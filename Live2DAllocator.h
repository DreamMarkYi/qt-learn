#ifndef LIVE2DALLOCATOR_HPP
#define LIVE2DALLOCATOR_HPP

#include <ICubismAllocator.hpp>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// 基于内存池的 Framework 分配器。
//
// 为什么用「分级 free-list 池」而不是「定长块池」：
// Cubism 的分配几乎全集中在加载期（解析 json / moc3 / 建顶点缓冲），运行时
// 每帧基本不分配；且请求大小跨度大、释放顺序任意。定长池套不进来，arena
// （只整体回收）又会在反复加载/卸载模型时无限增长。所以折中用分级 free-list：
//   - 小块(<= kMaxPooled)按 size class 切分、回收复用，省掉反复向 CRT 申请；
//   - 大块、或对齐要求超过 kAlign 的请求，直接走（对齐）malloc；
//   - 每个返回指针前藏 16 字节 Header，记录它从哪条路径来，释放时照此归位。
//
// 线程：加了一把 mutex 保证任意线程安全。若确认只在渲染线程分配，可去掉。
class Live2DAllocator : public Live2D::Cubism::Framework::ICubismAllocator
{
public:
    Live2DAllocator() = default;
    ~Live2DAllocator() override
    {
        // 进程退出时归还所有 slab；大块由 Framework Dispose 时各自 free。
        for (void* slab : _slabs) std::free(slab);
    }

    void* Allocate(const Live2D::Cubism::Framework::csmSizeType size) override
    {
        return allocate(size, kAlign);          // 统一按 kAlign 对齐，过度对齐无害
    }
    void Deallocate(void* memory) override { deallocate(memory); }

    void* AllocateAligned(const Live2D::Cubism::Framework::csmSizeType size,
                          const Live2D::Cubism::Framework::csmUint32 alignment) override
    {
        return allocate(size, alignment < kAlign ? kAlign : alignment);
    }
    void DeallocateAligned(void* alignedMemory) override { deallocate(alignedMemory); }

private:
    static constexpr std::size_t kAlign     = 16;        // 基础对齐（够 SIMD 顶点用）
    static constexpr std::size_t kHeader    = 16;        // Header 占位，且是 kAlign 倍数
    static constexpr std::size_t kSlabSize  = 64 * 1024; // 每次向 CRT 批发的大块
    static constexpr std::size_t kClasses[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    static constexpr std::size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
    static constexpr std::size_t kMaxPooled = 4096;

    enum Kind : std::uint32_t { POOL = 0, MALLOC = 1 };
    struct Header {                 // 藏在每个返回指针前 16 字节里
        std::uint32_t kind;
        std::uint32_t cls;          // POOL: size class 下标
        void*         origin;       // POOL: 块首；MALLOC: malloc 原始指针
    };

    static int classFor(std::size_t need)   // need 向上取到最近一档，超档返回 -1
    {
        for (int i = 0; i < (int)kClassCount; ++i)
            if (need <= kClasses[i]) return i;
        return -1;
    }

    void* allocate(std::size_t size, std::size_t alignment)
    {
        std::lock_guard<std::mutex> lk(_mtx);

        // —— 小块且只需基础对齐：走池 ——
        if (alignment <= kAlign) {
            int cls = classFor(size + kHeader);
            if (cls >= 0) {
                void* block = popOrCarve(cls);          // block 必为 16 对齐
                if (!block) return nullptr;
                Header* h = static_cast<Header*>(block);
                h->kind = POOL; h->cls = (std::uint32_t)cls; h->origin = block;
                return static_cast<std::uint8_t*>(block) + kHeader;
            }
        }

        // —— 大块 / 超对齐：对齐 malloc，Header 紧贴用户指针之前 ——
        std::size_t raw = size + alignment + kHeader;
        void* p = std::malloc(raw);
        if (!p) return nullptr;
        std::uintptr_t base    = reinterpret_cast<std::uintptr_t>(p) + kHeader;
        std::uintptr_t aligned = (base + alignment - 1) & ~(std::uintptr_t)(alignment - 1);
        Header* h = reinterpret_cast<Header*>(aligned - kHeader);
        h->kind = MALLOC; h->origin = p;
        return reinterpret_cast<void*>(aligned);
    }

    void deallocate(void* memory)
    {
        if (!memory) return;
        std::lock_guard<std::mutex> lk(_mtx);
        Header* h = reinterpret_cast<Header*>(
            static_cast<std::uint8_t*>(memory) - kHeader);
        if (h->kind == POOL) pushFree(h->cls, h->origin);  // 回收进对应 free list
        else                 std::free(h->origin);         // 还原始指针给 CRT
    }

    // free list 为侵入式单链：空闲块头 8 字节存 next 指针。
    void* popOrCarve(int cls)
    {
        if (!_free[cls]) carveSlab(cls);                   // 没了就开新 slab 切分
        void* block = _free[cls];
        if (!block) return nullptr;                        // OOM
        std::memcpy(&_free[cls], block, sizeof(void*));
        return block;
    }

    void carveSlab(int cls)
    {
        std::size_t blk = kClasses[cls];
        void* slab = std::malloc(kSlabSize);
        if (!slab) return;
        _slabs.push_back(slab);
        // slab 首地址向上对齐到 kAlign，保证切出的每块都 16 对齐
        std::uintptr_t p   = reinterpret_cast<std::uintptr_t>(slab);
        std::uintptr_t cur = (p + kAlign - 1) & ~(std::uintptr_t)(kAlign - 1);
        std::uintptr_t end = p + kSlabSize;
        while (cur + blk <= end) {                         // 逐块串进 free list
            std::memcpy(reinterpret_cast<void*>(cur), &_free[cls], sizeof(void*));
            _free[cls] = reinterpret_cast<void*>(cur);
            cur += blk;
        }
    }

    void pushFree(std::uint32_t cls, void* block)
    {
        std::memcpy(block, &_free[cls], sizeof(void*));
        _free[cls] = block;
    }

    std::mutex         _mtx;
    void*              _free[kClassCount] = {};
    std::vector<void*> _slabs;
};

#endif // LIVE2DALLOCATOR_HPP