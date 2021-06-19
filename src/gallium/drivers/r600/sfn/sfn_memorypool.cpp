#include "sfn_memorypool.h"

#include <cassert>
#include <iostream>

namespace r600 {

struct MemoryPoolImpl {
public:
   MemoryPoolImpl();
   ~MemoryPoolImpl();

   using MemoryBacking = ::std::pmr::monotonic_buffer_resource;

   MemoryBacking *pool;
};

MemoryPool::MemoryPool() noexcept : impl(nullptr)
{
}

MemoryPool& MemoryPool::instance()
{
    static thread_local MemoryPool me;
    me.initialize();
    return me;
}

void MemoryPool::free()
{
   delete impl;
   impl = nullptr;
}

void MemoryPool::initialize()
{
   if (!impl)
      impl = new MemoryPoolImpl();
}

void *MemoryPool::allocate(size_t size)
{
   return impl->pool->allocate(size);
}

void *MemoryPool::allocate(size_t size, size_t align)
{
   return impl->pool->allocate(size, align);
}

void MemoryPool::release_all()
{
   instance().free();
}

void init_pool()
{
    MemoryPool::instance();
}

void release_pool()
{
    MemoryPool::release_all();
}

void *Allocate::operator new(size_t size)
{
    return MemoryPool::instance().allocate(size);
}

void Allocate::operator delete (void *p, size_t size)
{
    // MemoryPool::instance().deallocate(p, size);
}

MemoryPoolImpl::MemoryPoolImpl()
{
   pool = new MemoryBacking();
}

MemoryPoolImpl::~MemoryPoolImpl()
{   
   delete pool;
}

}
