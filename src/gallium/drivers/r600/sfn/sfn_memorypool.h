#ifndef MEMORYPOOL_H
#define MEMORYPOOL_H

#include <cstdlib>
#include <memory>
#include <stack>

#if __cplusplus >= 21703L
#include <memory_resource>
#define R600_POINTER_TYPE(X) X *
#else
#error Need C++17
#endif

namespace r600  {

void init_pool();
void release_pool();

class Allocate
{
public:
    void * operator new(size_t size);
    void operator delete (void *p, size_t size);
};

class MemoryPool  {
public:
    static MemoryPool& instance();
    static void release_all();

    void free();
    void initialize();

    void *allocate(size_t size);
    void *allocate(size_t size, size_t align);

private:
    MemoryPool() noexcept;

    struct MemoryPoolImpl* impl;
};

template <typename T>
struct Allocator {
   using value_type = T;

   Allocator() = default;
   Allocator(const Allocator& other) = default;

   template <typename U>
   Allocator(const Allocator<U>& other) {(void)other;}

   T *allocate(size_t n) {
      return (T *)MemoryPool::instance().allocate(n * sizeof(T), alignof(T));
   }

   void deallocate(void *p, size_t n) {
      (void)p; (void)n;
      //MemoryPool::instance().deallocate(p, n * sizeof(T), alignof(T));
   }

   friend bool operator == (const Allocator<T>& lhs, const Allocator<T>& rhs) {
      (void)lhs; (void)rhs; return true;}
};

}

#endif // MEMORYPOOL_H
