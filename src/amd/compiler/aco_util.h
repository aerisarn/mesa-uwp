/*
 * Copyright Michael Schellenberger Costa
 * Copyright © 2020 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef ACO_UTIL_H
#define ACO_UTIL_H

#include "util/bitscan.h"
#include "util/u_math.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <unordered_map>
#include <vector>

namespace aco {

/*! \brief      Definition of a span object
 *
 *   \details    A "span" is an "array view" type for holding a view of contiguous
 *               data. The "span" object does not own the data itself.
 */
template <typename T> class span {
public:
   using value_type = T;
   using pointer = value_type*;
   using const_pointer = const value_type*;
   using reference = value_type&;
   using const_reference = const value_type&;
   using iterator = pointer;
   using const_iterator = const_pointer;
   using reverse_iterator = std::reverse_iterator<iterator>;
   using const_reverse_iterator = std::reverse_iterator<const_iterator>;
   using size_type = uint16_t;
   using difference_type = std::ptrdiff_t;

   /*! \brief                  Compiler generated default constructor
    */
   constexpr span() = default;

   /*! \brief                 Constructor taking a pointer and the length of the span
    *  \param[in]   data      Pointer to the underlying data array
    *  \param[in]   length    The size of the span
    */
   constexpr span(uint16_t offset_, const size_type length_) : offset{offset_}, length{length_} {}

   /*! \brief                 Returns an iterator to the begin of the span
    *  \return                data
    */
   constexpr iterator begin() noexcept { return (pointer)((uintptr_t)this + offset); }

   /*! \brief                 Returns a const_iterator to the begin of the span
    *  \return                data
    */
   constexpr const_iterator begin() const noexcept
   {
      return (const_pointer)((uintptr_t)this + offset);
   }

   /*! \brief                 Returns an iterator to the end of the span
    *  \return                data + length
    */
   constexpr iterator end() noexcept { return std::next(begin(), length); }

   /*! \brief                 Returns a const_iterator to the end of the span
    *  \return                data + length
    */
   constexpr const_iterator end() const noexcept { return std::next(begin(), length); }

   /*! \brief                 Returns a const_iterator to the begin of the span
    *  \return                data
    */
   constexpr const_iterator cbegin() const noexcept { return begin(); }

   /*! \brief                 Returns a const_iterator to the end of the span
    *  \return                data + length
    */
   constexpr const_iterator cend() const noexcept { return std::next(begin(), length); }

   /*! \brief                 Returns a reverse_iterator to the end of the span
    *  \return                reverse_iterator(end())
    */
   constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

   /*! \brief                 Returns a const_reverse_iterator to the end of the span
    *  \return                reverse_iterator(end())
    */
   constexpr const_reverse_iterator rbegin() const noexcept
   {
      return const_reverse_iterator(end());
   }

   /*! \brief                 Returns a reverse_iterator to the begin of the span
    *   \return                reverse_iterator(begin())
    */
   constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

   /*! \brief                 Returns a const_reverse_iterator to the begin of the span
    *  \return                reverse_iterator(begin())
    */
   constexpr const_reverse_iterator rend() const noexcept
   {
      return const_reverse_iterator(begin());
   }

   /*! \brief                 Returns a const_reverse_iterator to the end of the span
    *  \return                rbegin()
    */
   constexpr const_reverse_iterator crbegin() const noexcept
   {
      return const_reverse_iterator(cend());
   }

   /*! \brief                 Returns a const_reverse_iterator to the begin of the span
    *  \return                rend()
    */
   constexpr const_reverse_iterator crend() const noexcept
   {
      return const_reverse_iterator(cbegin());
   }

   /*! \brief                 Unchecked access operator
    *  \param[in] index       Index of the element we want to access
    *  \return                *(std::next(data, index))
    */
   constexpr reference operator[](const size_type index) noexcept
   {
      assert(length > index);
      return *(std::next(begin(), index));
   }

   /*! \brief                 Unchecked const access operator
    *  \param[in] index       Index of the element we want to access
    *  \return                *(std::next(data, index))
    */
   constexpr const_reference operator[](const size_type index) const noexcept
   {
      assert(length > index);
      return *(std::next(begin(), index));
   }

   /*! \brief                 Returns a reference to the last element of the span
    *  \return                *(std::next(data, length - 1))
    */
   constexpr reference back() noexcept
   {
      assert(length > 0);
      return *(std::next(begin(), length - 1));
   }

   /*! \brief                 Returns a const_reference to the last element of the span
    *  \return                *(std::next(data, length - 1))
    */
   constexpr const_reference back() const noexcept
   {
      assert(length > 0);
      return *(std::next(begin(), length - 1));
   }

   /*! \brief                 Returns a reference to the first element of the span
    *  \return                *begin()
    */
   constexpr reference front() noexcept
   {
      assert(length > 0);
      return *begin();
   }

   /*! \brief                 Returns a const_reference to the first element of the span
    *  \return                *cbegin()
    */
   constexpr const_reference front() const noexcept
   {
      assert(length > 0);
      return *cbegin();
   }

   /*! \brief                 Returns true if the span is empty
    *  \return                length == 0
    */
   constexpr bool empty() const noexcept { return length == 0; }

   /*! \brief                 Returns the size of the span
    *  \return                length == 0
    */
   constexpr size_type size() const noexcept { return length; }

   /*! \brief                 Decreases the size of the span by 1
    */
   constexpr void pop_back() noexcept
   {
      assert(length > 0);
      --length;
   }

   /*! \brief                 Adds an element to the end of the span
    */
   constexpr void push_back(const_reference val) noexcept { *std::next(begin(), length++) = val; }

   /*! \brief                 Clears the span
    */
   constexpr void clear() noexcept
   {
      offset = 0;
      length = 0;
   }

private:
   uint16_t offset{0};  //!> Byte offset from span to data
   size_type length{0}; //!> Size of the span
};

/*
 * Cache-friendly set of 32-bit IDs with fast insert/erase/lookup and
 * the ability to efficiently iterate over contained elements.
 *
 * Internally implemented as a map of fixed-size bit vectors: If the set contains an ID, the
 * corresponding bit in the appropriate bit vector is set. It doesn't use std::vector<bool> since
 * we then couldn't efficiently iterate over the elements.
 *
 * The interface resembles a subset of std::set/std::unordered_set.
 */
struct IDSet {
   static const uint32_t block_size = 1024u;
   using block_t = std::array<uint64_t, block_size / 64>;

   struct Iterator {
      const IDSet* set;
      std::map<uint32_t, block_t>::const_iterator block;
      uint32_t id;

      Iterator& operator++();

      bool operator!=(const Iterator& other) const;

      uint32_t operator*() const;
   };

   size_t count(uint32_t id) const { return find(id) != end(); }

   Iterator find(uint32_t id) const
   {
      uint32_t block_index = id / block_size;
      auto it = words.find(block_index);
      if (it == words.end())
         return end();

      const block_t& block = it->second;
      uint32_t sub_id = id % block_size;

      if (block[sub_id / 64u] & (1ull << (sub_id % 64u)))
         return Iterator{this, it, id};
      else
         return end();
   }

   std::pair<Iterator, bool> insert(uint32_t id)
   {
      uint32_t block_index = id / block_size;
      auto it = words.try_emplace(block_index).first;
      block_t& block = it->second;
      uint32_t sub_id = id % block_size;

      uint64_t* word = &block[sub_id / 64u];
      uint64_t mask = 1ull << (sub_id % 64u);
      if (*word & mask)
         return std::make_pair(Iterator{this, it, id}, false);

      *word |= mask;
      return std::make_pair(Iterator{this, it, id}, true);
   }

   bool insert(const IDSet other)
   {
      bool inserted = false;

      for (auto it = other.words.begin(); it != other.words.end(); ++it) {
         block_t& dst = words[it->first];
         const block_t& src = it->second;

         for (unsigned j = 0; j < src.size(); j++) {
            uint64_t new_bits = src[j] & ~dst[j];
            if (new_bits) {
               inserted = true;
               dst[j] |= new_bits;
            }
         }
      }
      return inserted;
   }

   size_t erase(uint32_t id)
   {
      uint32_t block_index = id / block_size;
      auto it = words.find(block_index);
      if (it == words.end())
         return 0;

      block_t& block = it->second;
      uint32_t sub_id = id % block_size;

      uint64_t* word = &block[sub_id / 64u];
      uint64_t mask = 1ull << (sub_id % 64u);
      if (!(*word & mask))
         return 0;

      *word ^= mask;
      return 1;
   }

   Iterator cbegin() const
   {
      Iterator res;
      res.set = this;

      for (auto it = words.begin(); it != words.end(); ++it) {
         uint32_t first = get_first_set(it->second);
         if (first != UINT32_MAX) {
            res.block = it;
            res.id = it->first * block_size + first;
            return res;
         }
      }

      return cend();
   }

   Iterator cend() const { return Iterator{this, words.end(), UINT32_MAX}; }

   Iterator begin() const { return cbegin(); }

   Iterator end() const { return cend(); }

   size_t size() const
   {
      size_t bits_set = 0;
      for (auto block : words) {
         for (uint64_t word : block.second)
            bits_set += util_bitcount64(word);
      }
      return bits_set;
   }

   bool empty() const { return !size(); }

private:
   static uint32_t get_first_set(const block_t& words)
   {
      for (size_t i = 0; i < words.size(); i++) {
         if (words[i])
            return i * 64u + (ffsll(words[i]) - 1);
      }
      return UINT32_MAX;
   }

   std::map<uint32_t, block_t> words;
};

inline IDSet::Iterator&
IDSet::Iterator::operator++()
{
   uint32_t block_index = id / block_size;
   const block_t& block_words = block->second;
   uint32_t sub_id = id % block_size;

   uint64_t m = block_words[sub_id / 64u];
   uint32_t bit = sub_id % 64u;
   m = (m >> bit) >> 1;
   if (m) {
      id += ffsll(m);
      return *this;
   }

   for (uint32_t i = sub_id / 64u + 1; i < block_words.size(); i++) {
      if (block_words[i]) {
         id = block_index * block_size + i * 64u + ffsll(block_words[i]) - 1;
         return *this;
      }
   }

   for (++block; block != set->words.end(); ++block) {
      uint32_t first = get_first_set(block->second);
      if (first != UINT32_MAX) {
         id = block->first * block_size + first;
         return *this;
      }
   }

   id = UINT32_MAX;
   return *this;
}

inline bool
IDSet::Iterator::operator!=(const IDSet::Iterator& other) const
{
   assert(set == other.set);
   assert(id != other.id || block == other.block);
   return id != other.id;
}

inline uint32_t
IDSet::Iterator::operator*() const
{
   return id;
}

/*
 * Light-weight memory resource which allows to sequentially allocate from
 * a buffer. Both, the release() method and the destructor release all managed
 * memory.
 *
 * The memory resource is not thread-safe.
 * This class mimics std::pmr::monotonic_buffer_resource
 */
class monotonic_buffer_resource final {
public:
   explicit monotonic_buffer_resource(size_t size = initial_size)
   {
      /* The size parameter refers to the total size of the buffer.
       * The usable data_size is size - sizeof(Buffer).
       */
      size = MAX2(size, minimum_size);
      buffer = (Buffer*)malloc(size);
      buffer->next = nullptr;
      buffer->data_size = size - sizeof(Buffer);
      buffer->current_idx = 0;
   }

   ~monotonic_buffer_resource()
   {
      release();
      free(buffer);
   }

   /* Delete copy-constructor and -assigment to avoid double free() */
   monotonic_buffer_resource(const monotonic_buffer_resource&) = delete;
   monotonic_buffer_resource& operator=(const monotonic_buffer_resource&) = delete;

   void* allocate(size_t size, size_t alignment)
   {
      buffer->current_idx = align(buffer->current_idx, alignment);
      if (buffer->current_idx + size <= buffer->data_size) {
         uint8_t* ptr = &buffer->data[buffer->current_idx];
         buffer->current_idx += size;
         return ptr;
      }

      /* create new larger buffer */
      uint32_t total_size = buffer->data_size + sizeof(Buffer);
      do {
         total_size *= 2;
      } while (total_size - sizeof(Buffer) < size);
      Buffer* next = buffer;
      buffer = (Buffer*)malloc(total_size);
      buffer->next = next;
      buffer->data_size = total_size - sizeof(Buffer);
      buffer->current_idx = 0;

      return allocate(size, alignment);
   }

   void release()
   {
      while (buffer->next) {
         Buffer* next = buffer->next;
         free(buffer);
         buffer = next;
      }
      buffer->current_idx = 0;
   }

   bool operator==(const monotonic_buffer_resource& other) { return buffer == other.buffer; }

private:
   struct Buffer {
      Buffer* next;
      uint32_t current_idx;
      uint32_t data_size;
      uint8_t data[];
   };

   Buffer* buffer;
   static constexpr size_t initial_size = 4096;
   static constexpr size_t minimum_size = 128;
   static_assert(minimum_size > sizeof(Buffer));
};

/*
 * Small memory allocator which wraps monotonic_buffer_resource
 * in order to implement <allocator_traits>.
 *
 * This class mimics std::pmr::polymorphic_allocator with monotonic_buffer_resource
 * as memory resource. The advantage of this specialization is the absence of
 * virtual function calls and the propagation on swap, copy- and move assignment.
 */
template <typename T> class monotonic_allocator final {
public:
   monotonic_allocator() = delete;
   monotonic_allocator(monotonic_buffer_resource& m) : memory_resource(m) {}
   template <typename U>
   explicit monotonic_allocator(const monotonic_allocator<U>& rhs)
       : memory_resource(rhs.memory_resource)
   {}

   /* Memory Allocation */
   T* allocate(size_t size)
   {
      uint32_t bytes = sizeof(T) * size;
      return (T*)memory_resource.get().allocate(bytes, alignof(T));
   }

   /* Memory will be freed on destruction of memory_resource */
   void deallocate(T* ptr, size_t size) {}

   /* Implement <allocator_traits> */
   using value_type = T;
   template <class U> struct rebind {
      using other = monotonic_allocator<U>;
   };

   typedef std::true_type propagate_on_container_copy_assignment;
   typedef std::true_type propagate_on_container_move_assignment;
   typedef std::true_type propagate_on_container_swap;

   template <typename> friend class monotonic_allocator;
   template <typename X, typename Y>
   friend bool operator==(monotonic_allocator<X> const& a, monotonic_allocator<Y> const& b);
   template <typename X, typename Y>
   friend bool operator!=(monotonic_allocator<X> const& a, monotonic_allocator<Y> const& b);

private:
   std::reference_wrapper<monotonic_buffer_resource> memory_resource;
};

/* Necessary for <allocator_traits>. */
template <typename X, typename Y>
inline bool
operator==(monotonic_allocator<X> const& a, monotonic_allocator<Y> const& b)
{
   return a.memory_resource.get() == b.memory_resource.get();
}
template <typename X, typename Y>
inline bool
operator!=(monotonic_allocator<X> const& a, monotonic_allocator<Y> const& b)
{
   return !(a == b);
}

/*
 * aco::map - alias for std::map with monotonic_allocator
 *
 * This template specialization mimics std::pmr::map.
 */
template <class Key, class T, class Compare = std::less<Key>>
using map = std::map<Key, T, Compare, aco::monotonic_allocator<std::pair<const Key, T>>>;

/*
 * aco::unordered_map - alias for std::unordered_map with monotonic_allocator
 *
 * This template specialization mimics std::pmr::unordered_map.
 */
template <class Key, class T, class Hash = std::hash<Key>, class Pred = std::equal_to<Key>>
using unordered_map =
   std::unordered_map<Key, T, Hash, Pred, aco::monotonic_allocator<std::pair<const Key, T>>>;

} // namespace aco

#endif // ACO_UTIL_H
