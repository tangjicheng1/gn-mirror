// Copyright (c) 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_GN_HASH_TABLE_BASE_H_
#define TOOLS_GN_HASH_TABLE_BASE_H_

#include "base/compiler_specific.h"

// IMPORTANT DISCLAIMER:
//
// THIS IS *NOT* A GENERAL PURPOSE HASH TABLE TEMPLATE. INSTEAD, IT CAN
// CAN BE USED AS THE BASIS FOR VERY HIGH SPEED AND COMPACT HASH TABLES
// THAT OBEY VERY STRICT CONDITIONS DESCRIBED BELOW.
//
// DO NOT USE THIS UNLESS YOU HAVE A GOOD REASON, I.E. THAT PROFILING
// SHOWS THERE *IS* AN OVERALL BENEFIT TO DO SO. FOR MOST CASES,
// std::set<>, std::unordered_set<> and base::flat_set<> ARE PERFECTLY
// FINE AND SHOULD BE PREFERRED.
//
//
// That being said, this implementation uses a completely typical
// open-addressing scheme with a buckets array size which is always
// a power of 2, instead of a prime number. Experience shows this is
// not detrimental to performance as long as you have a sufficiently
// good hash function (which is the case of all C++ standard libraries
// these days for strings and pointers).
//
// The reason it is so fast is due to its compactness and the very
// simple but tight code for a typical lookup / insert / deletion
// operation.
//
// The |buckets_| field holds a pointer to an array of NODE_TYPE
// instances, called nodes. Each node represents one of the following:
// a free entry in the table, an inserted item, or a tombstone marking
// the location of a previously deleted item. Tombstones are only
// used if the hash table instantiation requires deletion support
// (see the is_tombstone() description below).
//
// The template requires that Node be a type with the following traits:
//
//  - It *must* be a trivial type, that is zero-initialized.
//
//  - It provides an is_null() const method, which should return true
//    iff the corresponding node matches a 'free' entry in the hash
//    table, i.e. one that has not been assigned to an item, or
//    to a deletion tombstone (see below).
//
//    Of course, a default (zeroed) value, should always return true.
//
//  - It provides an is_tombstone() const method, which should return
//    return true iff the corresponding node indicates a previously
//    deleted item.
//
//    Note that if you hash table does not need deletion support,
//    it is highly recommended to make this a static constexpr method
//    that always return false. Doing so will optimize the lookup loop
//    automatically!
//
//  - It must provide an is_valid() method, that simply returns
//    (!is_null() && !is_tombstone()). This is a convenience for this
//    template, but also for the derived class that will extend it
//    (more on this below, in the usage description).
//
// Note that because Node instances are trivial, std::unique_ptr<>
// cannot be used in them. Item lifecycle must this be managed
// explicitly by a derived class of the template instantiation
// instead.
//
// Lookup, insertion and deletion are performed in ways that
// are *very* different from standard containers, and the reason
// is, unsuprisingly, performance.
//
// Use NodeLookup() to look for an existing item in the hash table.
// This takes the item's hash value, and a templated callable to
// compare a node against the search key. This scheme allows
// heterogeneous lookups, and keeps the node type details
// out of this header. Any recent C++ optimizer will generate
// very tight machine code for this call.
//
// The NodeLookup() function always returns a non-null and
// mutable |node| pointer. If |node->is_valid()| is true,
// then the item was found, and |node| points to it.
//
// Otherwise, |node| corresponds to a location that may be
// used for insertion. To do so, the caller should update the
// content of |node| appropriately (e.g. writing a pointer and/or
// hash value to it), then call UpdateAfterInsertion(), which
// may eventually grow the table and rehash nodes in it.
//
// To delete an item, call NodeLookup() first to verify that
// the item is present, then write a tombstone value to |node|,
// then call UpdateAfterDeleetion().
//
// Note that what the tombstone value is doesn't matter to this
// header, as long as |node->is_tombstone()| returns true for
// it.
//
// Here's an example of a concrete implementation that stores
// a hash value and an owning pointer in each node:
//
//     struct MyFooNode {
//       size_t hash;
//       Foo*   foo;
//
//       bool is_null() const { return !foo; }
//       bool is_tombstone() const { return foo == &kTombstone; }
//       bool is_valid() const { return !is_null() && !is_tombstone(); }
//
//       static const Foo kTombstone;
//     };
//
//    class MyFooSet : public HashTableBase<MyFoodNode> {
//    public:
//      using BaseType = HashTableBase<MyFooNode>;
//      using Node = BaseType::Node;
//
//      ~MyFooSet() {
//        // Destroy all items in the set.
//        // Note that the iterator only parses over valid items.
//        for (Node* node : *this) {
//          delete node->foo;
//        }
//      }
//
//      // Returns true iff this set contains |key|.
//      bool contains(const Foo& key) const {
//        Node* node = BaseType::NodeLookup(
//            MakeHash(key),
//            [&](const Node* node) { return node->foo == key; });
//
//        return node->is_valid();
//      }
//
//      // Try to add |key| to the set. Return true if the insertion
//      // was succesful, or false if the item was already in the set.
//      bool add(const Foo& key) const {
//        size_t hash = MakeHash(key);
//        Node* node = NodeLookup(
//            hash,
//            [&](const Node* node) { return node->foo == key; });
//
//        if (node->is_valid()) {
//          // Already in the set.
//          return false;
//        }
//
//        // Add it.
//        node->hash = hash;
//        node->foo  = new Foo(key);
//        UpdateAfterInsert();
//        return true;
//      }
//
//      // Try to remove |key| from the set. Return true if the
//      // item was already in it, false otherwise.
//      bool erase(const Foo& key) const {
//        Node* node = BaseType::NodeLookup(
//            MakeHash(key),
//            [&](const Node* node) { return node->foo == key; });
//
//        if (!node->is_valid()) {
//          // Not in the set.
//          return false;
//        }
//
//        delete node->foo;
//        node->foo = Node::kTombstone;
//        UpdateAfterDeletion().
//      }
//
//      static size_t MakeHash(const Foo& foo) {
//        return std::hash<Foo>()();
//      }
//    };
//
// For more concrete examples, see the implementation of
// StringAtom or UniqueVector<>
//
template <typename NODE_TYPE>
class HashTableBase {
 public:
  using Node = NODE_TYPE;

  static_assert(std::is_trivial<NODE_TYPE>::value,
                "KEY_TYPE should be a trivial type!");

  static_assert(std::is_standard_layout<NODE_TYPE>::value,
                "KEY_TYPE should be a standard layout type!");

  // Default constructor.
  HashTableBase() = default;

  // Destructor.
  ~HashTableBase() {
    if (buckets_ != buckets0_)
      free(buckets_);
  }

  // Copy and move operations
  HashTableBase(const HashTableBase& other)
      : count_(other.count_), size_(other.size_) {
    if (other.buckets_ == other.buckets0_)
      buckets_ = reinterpret_cast<Node*>(calloc(other.size_), sizeof(Node));
    memcpy(buckets_, other.buckets_, other.size_, sizeof(Node));
  }

  HashTableBase& operator=(const HashTableBase& other) {
    if (this != &other) {
      this->~HashTableBase();
      new (this) HashTableBase(other);
    }
    return *this;
  }

  HashTableBase(HashTableBase&& other) noexcept
      : count_(other.size_), size_(other.size_), buckets_(other.buckets_) {
    if (buckets_ == other.buckets0_) {
      buckets0_[0] = other.buckets0_[0];
    } else {
      other.buckets_ = other.buckets0_;
    }
    other.clear();
  }

  HashTableBase& operator=(HashTableBase&& other) noexcept {
    if (this != &other) {
      this->~HashTableBase();
      new (this) HashTableBase(std::move(other));
    }
    return *this;
  }

  // Return true iff the table is empty.
  bool empty() const { return count_ == 0; }

  // Return the number of keys in the set.
  size_t size() const { return count_; }

  // Clear the table completely.
  void Clear() {
    if (buckets_ != buckets0_)
      free(buckets_);

    count_ = 0;
    size_ = 1;
    buckets_ = buckets0_;
    buckets0_[0] = Node{};
  }

  // Iteration support.
  struct iterator {
    using reference = Node&;

    reference operator*() const { return *node_; }

    bool operator==(const iterator& other) const {
      return node_ == other.node_;
    }

    bool operator!=(const iterator& other) const {
      return node_ != other.node_;
    }

    // pre-increment
    iterator& operator++() {
      node_++;
      while (node_ < node_limit_ && !node_->is_valid())
        node_++;

      return *this;
    }

    // post-increment
    iterator operator++(int) {
      iterator result = *this;
      ++(*this);
      return result;
    }

    Node* node_;
    Node* node_limit_;
  };

  iterator begin() {
    Node* node = buckets_;
    Node* limit = node + size_;
    while (node < limit && !node->is_valid())
      node++;

    return {node, limit};
  }

  iterator end() {
    Node* limit = buckets_ + size_;
    return {limit, limit};
  }

  // Constant iteration support.
  struct const_iterator {
    using const_reference = const Node&;

    const_reference operator*() const { return *node_; }

    bool operator==(const const_iterator& other) const {
      return node_ == other.node_;
    }

    bool operator!=(const const_iterator& other) const {
      return node_ != other.node_;
    }

    // pre-increment
    const_iterator& operator++() {
      node_++;
      while (node_ < node_limit_ && !node_->is_valid())
        node_++;

      return *this;
    }

    // post-increment
    const_iterator operator++(int) {
      const_iterator result = *this;
      ++(*this);
      return result;
    }

    const Node* node_;
    const Node* node_limit_;
  };

  const_iterator begin() const { return cbegin(); }

  const_iterator end() const { return cend(); }

  const_iterator cbegin() const {
    const Node* node = buckets_;
    const Node* limit = node + size_;
    while (node < limit && !node->is_valid())
      node++;

    return {node, limit};
  }

  const_iterator cend() const {
    const Node* limit = buckets_ + size_;
    return {limit, limit};
  }

 protected:
  // The following should only be called by derived classes that
  // extend this template class, and are not available to their
  // clients. This forces the derived class to implement lookup
  // insertion and deletion with sane APIs instead.

  iterator IteratorFromNode(const Node* node) {
    return {node, buckets_ + size_};
  }

  const_iterator IteratorFromNode(const Node* node) const {
    return {node, buckets_ + size_};
  }

  // Lookup for a node in the hash table that matches the |node_equal|
  // predicate, which takes a const Node* pointer argument, and returns
  // true iff this corresponds to a lookup match.
  //
  // IMPORTANT: |node_equal| may or may not check the node hash value,
  // the choice is left to the implementation.
  //
  // Returns a non-null *mutable* node pointer. |node->is_valid()| will
  // be true for matches, and false for misses.
  template <typename NODE_EQUAL>
  Node* NodeLookup(size_t hash, NODE_EQUAL node_equal) const {
    size_t mask = size_ - 1;
    size_t index = hash & mask;
    const Node* tombstone = nullptr;  // First tombstone node found, if any.
    for (;;) {
      const Node* node = buckets_ + index;
      if (node->is_null()) {
        return const_cast<Node*>(tombstone ? tombstone : node);
      }
      if (node->is_tombstone()) {
        if (!tombstone)
          tombstone = node;
      } else if (node_equal(node)) {
        return const_cast<Node*>(node);
      }
      index = (index + 1) & mask;
    }
  }

  // Call this method after updating the content of the |node| pointer
  // returned by an unsucessful NodeLookup(). Return true to indicate that
  // the table size changed, and that existing iterators were invalidated.
  bool UpdateAfterInsert() {
    count_ += 1;
    if (UNLIKELY(count_ * 4 >= size_ * 3)) {
      GrowBuckets();
      return true;
    }
    return false;
  }

  // Call this method after updating the content of the |node| value
  // returned a by succesful NodeLookup, to the tombstone value, if any.
  // Return true to indicate a table size change, ie. that existing
  // iterators where invalidated.
  bool UpdateAfterRemoval() {
    count_ -= 1;
    // For now don't support shrinking since this is not useful for GN.
    return false;
  }

 private:
  [[gnu::noinline]] void GrowBuckets() {
    size_t size = size_;
    size_t new_size = (size == 1) ? 8 : size * 2;
    size_t new_mask = new_size - 1;

    Node* new_buckets = reinterpret_cast<Node*>(calloc(new_size, sizeof(Node)));

    for (size_t nn = 0; nn < size; ++nn) {
      const Node* node = &buckets_[nn];
      if (!node->is_valid())
        continue;
      size_t index = node->hash_value() & new_mask;
      for (;;) {
        Node* node2 = new_buckets + index;
        if (node2->is_null()) {
          *node2 = *node;
          break;
        }
        index = (index + 1) & new_mask;
      }
    }

    if (buckets_ != buckets0_)
      free(buckets_);

    buckets_ = new_buckets;
    size_ = new_size;
  }

  // NOTE: The reason for default-initializing |buckets_| to a storage slot
  // inside the object is to ensure the value is never null. This removes one
  // nullptr check from each NodeLookup() instantiation.
  size_t count_ = 0;
  size_t size_ = 1;
  Node* buckets_ = buckets0_;
  Node buckets0_[1] = {{}};
};

#endif  // TOOLS_GN_HASH_TABLE_BASE_H_
