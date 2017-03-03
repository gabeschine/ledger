// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/storage/impl/btree/btree_utils.h"

#include "apps/ledger/src/callback/asynchronous_callback.h"
#include "apps/ledger/src/callback/waiter.h"
#include "lib/ftl/functional/closure.h"
#include "lib/ftl/functional/make_copyable.h"
#include "third_party/murmurhash/murmurhash.h"

namespace storage {
namespace btree {
namespace {

constexpr uint32_t kMurmurHashSeed = 0xbeef;

using HashResultType = decltype(murmurhash(nullptr, 0, 0));
using HashSliceType = uint8_t;

union Hash {
  HashResultType hash;
  HashSliceType slices[sizeof(HashResultType) / sizeof(HashSliceType)];
};

static_assert(sizeof(Hash::slices) == sizeof(Hash::hash),
              "Hash size is incorrect.");
static_assert(sizeof(HashSliceType) < std::numeric_limits<uint8_t>::max(),
              "Hash size is too big.");

Hash FastHash(convert::ExtendedStringView value) {
  return {.hash = murmurhash(value.data(), value.size(), kMurmurHashSeed)};
}

uint8_t GetNodeLevel(convert::ExtendedStringView key) {
  // Compute the level of a key by computing the hash of the key.
  // A key is at level k if the first k bytes of the hash of |key| are 0s. This
  // constructs a tree with an expected node size of |255|.
  Hash hash = FastHash(key);
  for (size_t l = 0; l < sizeof(Hash); ++l) {
    if (hash.slices[l]) {
      return l;
    }
  }
  return std::numeric_limits<uint8_t>::max();
}

constexpr NodeLevelCalculator kDefaultNodeLevelCalculator = {&GetNodeLevel};

// Helper functions for btree::ForEach.
void ForEachEntryInSubtree(PageStorage* page_storage,
                           std::unique_ptr<const TreeNode> node,
                           std::string min_key,
                           std::function<bool(EntryAndNodeId)> on_next,
                           std::function<void(Status, bool)> on_done);

// If |child_id| is not empty, calls |on_done| with the TreeNode corresponding
// to the id. Otherwise, calls |on_done| with Status::NO_SUCH_CHILD and nullptr.
void FindChild(
    PageStorage* page_storage,
    ObjectIdView child_id,
    std::function<void(Status, std::unique_ptr<const TreeNode>)> on_done) {
  if (child_id.empty()) {
    on_done(Status::NO_SUCH_CHILD, nullptr);
    return;
  }
  TreeNode::FromId(page_storage, child_id, std::move(on_done));
}

// Recursively iterates throught the child nodes and entries of |parent|
// starting at |index|. |on_done| is called with the return status and a bool
// indicating whether the iteration was interrupted by |on_next|.
void ForEachEntryInChildIndex(PageStorage* page_storage,
                              std::unique_ptr<const TreeNode> parent,
                              int index,
                              std::string min_key,
                              std::function<bool(EntryAndNodeId)> on_next,
                              std::function<void(Status, bool)> on_done) {
  if (index == parent->GetKeyCount() + 1) {
    on_done(Status::OK, false);
    return;
  }
  // First, find the child at index.
  FindChild(page_storage, parent->GetChildId(index), ftl::MakeCopyable([
              page_storage, parent = std::move(parent), index,
              min_key = std::move(min_key), on_next = std::move(on_next),
              on_done = std::move(on_done)
            ](Status s, std::unique_ptr<const TreeNode> child) mutable {
              if (s != Status::OK && s != Status::NO_SUCH_CHILD) {
                on_done(s, false);
                return;
              }
              if (child == nullptr) {
                // If the child was not found in the search branch, no need to
                // search again.
                min_key = "";
              }
              // Then, finish iterating through the subtree of that child.
              ForEachEntryInSubtree(
                  page_storage, std::move(child), min_key, on_next,
                  ftl::MakeCopyable([
                    page_storage, parent = std::move(parent), index,
                    min_key = std::move(min_key), on_next = std::move(on_next),
                    on_done = std::move(on_done)
                  ](Status s, bool interrupted) mutable {
                    if (s != Status::OK || interrupted) {
                      on_done(s, interrupted);
                      return;
                    }
                    // Then, add the entry right after the child.
                    if (index != parent->GetKeyCount()) {
                      Entry entry;
                      FTL_CHECK(parent->GetEntry(index, &entry) == Status::OK);
                      EntryAndNodeId next{entry, parent->GetId()};
                      if (!on_next(next)) {
                        on_done(Status::OK, true);
                        return;
                      }
                    }
                    // Finally, continue the recursion at index + 1.
                    ForEachEntryInChildIndex(page_storage, std::move(parent),
                                             index + 1, std::move(min_key),
                                             std::move(on_next),
                                             std::move(on_done));
                  }));
            }));
}

// Performs an in-order traversal of the subtree having |node| as root and calls
// |on_next| on each entry found with a key equal to or greater than |min_key|.
// |on_done| is called with the return status and a bool indicating whether the
// iteration was interrupted by |on_next|.
void ForEachEntryInSubtree(PageStorage* page_storage,
                           std::unique_ptr<const TreeNode> node,
                           std::string min_key,
                           std::function<bool(EntryAndNodeId)> on_next,
                           std::function<void(Status, bool)> on_done) {
  if (node == nullptr) {
    on_done(Status::OK, false);
    return;
  }
  // Supposing that min_key = "35":
  //  [10, 30, 40, 70]                [10, 35, 40, 70]
  //         /    \                      /    \
  //   [32, 35]  [49, 50]          [22, 34]  [38, 39]
  // In the left tree's root node, "35" is not found and start_index will be 2,
  // i.e. continue search in child node at index 2.
  // In the right tree's root node, "35" is found and start_index will be 1,
  // i.e. call |on_next| for entry at index 1 ("35") and continue in child node
  // at 2.
  int start_index;
  Status key_found = node->FindKeyOrChild(min_key, &start_index);
  // If the key is found call on_next with the corresponding entry. Otherwise,
  // handle directly the next child, which is already pointed by start_index.
  if (key_found != Status::NOT_FOUND) {
    if (key_found != Status::OK) {
      on_done(key_found, false);
      return;
    }

    Entry entry;
    FTL_CHECK(node->GetEntry(start_index, &entry) == Status::OK);
    EntryAndNodeId next{entry, node->GetId()};
    if (!on_next(next)) {
      on_done(Status::OK, true);
      return;
    }
    // The child is found, no need to search again.
    min_key = "";
    ++start_index;
  }

  ForEachEntryInChildIndex(page_storage, std::move(node), start_index,
                           std::move(min_key), std::move(on_next),
                           std::move(on_done));
}

// Returns a vector with all the tree's entries, sorted by key.
void GetEntriesVector(
    PageStorage* page_storage,
    ObjectIdView root_id,
    std::function<void(Status, std::unique_ptr<std::vector<Entry>>)> on_done) {
  auto entries = std::make_unique<std::vector<Entry>>();
  auto on_next = [entries = entries.get()](EntryAndNodeId e) {
    entries->push_back(e.entry);
    return true;
  };
  btree::ForEachEntry(
      page_storage, root_id, "", on_next, ftl::MakeCopyable([
        entries = std::move(entries), on_done = std::move(on_done)
      ](Status s) mutable {
        if (s != Status::OK) {
          on_done(s, nullptr);
          return;
        }
        on_done(Status::OK, std::move(entries));
      }));
}

// A ref-counted vector. This allows to share entries and/or children between
// mutliple node builders.
template <typename A>
class RefVector : public ftl::RefCountedThreadSafe<RefVector<A>> {
 public:
  static ftl::RefPtr<RefVector<A>> Create(std::vector<A> vector = {}) {
    return AdoptRef(new RefVector<A>(std::move(vector)));
  }

  static ftl::RefPtr<RefVector<A>> Create(
      typename std::vector<A>::const_iterator begin,
      typename std::vector<A>::const_iterator end) {
    return Create(std::vector<A>(std::move(begin), std::move(end)));
  }

  bool empty() const { return vector_.empty(); }

  size_t size() const { return vector_.size(); }

  auto begin() const { return vector_.begin(); }

  auto end() const { return vector_.end(); }

  auto front() const { return vector_.front(); }

  auto back() const { return vector_.back(); }

  const A& Get(size_t index) const { return vector_[index]; }

  // Returns a copy of this vector truncated at |end|.
  ftl::RefPtr<RefVector<A>> CopyUntil(
      typename std::vector<A>::const_iterator end) const {
    return Create(begin(), end);
  }

  void push_back(A value) { vector_.push_back(std::move(value)); }

  // Append elements between |begin| and |end| to this vector.
  void Append(typename std::vector<A>::const_iterator begin,
              typename std::vector<A>::const_iterator end) {
    vector_.insert(vector_.end(), std::move(begin), std::move(end));
  }

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(RefVector<A>);

  explicit RefVector(std::vector<A> vector) : vector_(std::move(vector)) {}
  ~RefVector() {}

  std::vector<A> vector_;
};

// Base class for tree nodes during construction. To apply mutations on a tree
// node, once start by creating an instance of ExistingNodeBuilder from the id
// of an existing tree node, then applies mutation on it, getting a new
// NodeBuilder in a callback each time. Once all mutations are applies, a call
// to Build with build a TreeNode in the storage.
// TODO(qsr): LE-150 It should be possible to mutate Builder as a builder will
//            not be used anymore once it is mutated.
class NodeBuilder : public ftl::RefCountedThreadSafe<const NodeBuilder> {
 public:
  // Static version of |Build| that handles null nodes.
  static void Build(
      ftl::RefPtr<const NodeBuilder> node_builder,
      PageStorage* page_storage,
      std::function<void(Status,
                         std::pair<ObjectId, std::unordered_set<ObjectId>>)>
          callback) {
    if (!node_builder) {
      callback(Status::OK, {});
      return;
    }

    node_builder->Build(page_storage, std::move(callback));
  }

  // Static version of |Apply| that handles null nodes.
  static void Apply(
      ftl::RefPtr<const NodeBuilder> node_builder,
      const NodeLevelCalculator* node_level_calculator,
      PageStorage* page_storage,
      EntryChange change,
      std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback);

 protected:
  FRIEND_REF_COUNTED_THREAD_SAFE(const NodeBuilder);

  virtual ~NodeBuilder() {}
  NodeBuilder(uint8_t level) : level_(level) {}

  // Build the tree node represented by the builder |node_builder| in the
  // storage.
  virtual void Build(
      PageStorage* page_storage,
      std::function<void(Status,
                         std::pair<ObjectId, std::unordered_set<ObjectId>>)>
          callback) const = 0;

  // Returns the entries and children of this builder.
  virtual void GetContent(
      PageStorage* page_storage,
      std::function<void(
          Status,
          const ftl::RefPtr<const RefVector<Entry>>&,
          const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&)>
          callback) const = 0;

  uint8_t level_;

 private:
  FRIEND_REF_COUNTED_THREAD_SAFE(NodeBuilder);

  // Apply the given mutation on |node_builder|.
  void Apply(const NodeLevelCalculator* node_level_calculator,
             PageStorage* page_storage,
             EntryChange change,
             std::function<void(Status, ftl::RefPtr<const NodeBuilder>)>
                 callback) const;

  // Delete the value with the given |key| from the builder. |key_level| must be
  // greater or equal then the node level.
  void Delete(PageStorage* page_storage,
              uint8_t key_level,
              std::string key,
              std::function<void(Status, ftl::RefPtr<const NodeBuilder>)>
                  callback) const;

  // Update the tree by adding |entry| (or modifying the value associated to
  // |entry.key| with |entry.value| if |key| is already in the tree.
  // |change_level| must be greater or equal then the node level.
  void Update(PageStorage* page_storage,
              uint8_t change_level,
              Entry entry,
              std::function<void(Status, ftl::RefPtr<const NodeBuilder>)>
                  callback) const;

  // Split the current tree in 2 according to |key|. This method expects that
  // |key| is not in the tree.
  void Split(
      PageStorage* page_storage,
      std::string key,
      std::function<void(Status,
                         ftl::RefPtr<const NodeBuilder>,
                         ftl::RefPtr<const NodeBuilder>)> callback) const;

  // Merge this tree with |other|. This expects all elements of |other| to be
  // greather than elements in |this|.
  void Merge(PageStorage* page_storage,
             ftl::RefPtr<const NodeBuilder> other,
             std::function<void(Status, ftl::RefPtr<const NodeBuilder>)>
                 callback) const;

  // Add needed parent to this tree to produce a new tree of level
  // |target_level|.
  ftl::RefPtr<const NodeBuilder> ToLevel(uint8_t target_level) const;

  // Static version of |Split| that handles null nodes.
  static void Split(
      ftl::RefPtr<const NodeBuilder> node_builder,
      PageStorage* page_storage,
      std::string key,
      std::function<void(Status,
                         ftl::RefPtr<const NodeBuilder>,
                         ftl::RefPtr<const NodeBuilder>)> callback) {
    if (!node_builder) {
      callback(Status::OK, nullptr, nullptr);
      return;
    }
    node_builder->Split(page_storage, std::move(key), std::move(callback));
  }

  // Static version of |Merge| that handles null nodes.
  static void Merge(
      PageStorage* page_storage,
      ftl::RefPtr<const NodeBuilder> n1,
      ftl::RefPtr<const NodeBuilder> n2,
      std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback) {
    if (!n1) {
      callback(Status::OK, std::move(n2));
      return;
    }
    n1->Merge(page_storage, n2, std::move(callback));
  }

  // Static version of |ToLevel| that handles null nodes.
  static ftl::RefPtr<const NodeBuilder> ToLevel(
      ftl::RefPtr<const NodeBuilder> node_builder,
      uint8_t target_level) {
    if (node_builder) {
      return node_builder->ToLevel(target_level);
    }
    return nullptr;
  }
};

// A NodeBuilder that represents an already existing node.
class ExistingNodeBuilder : public NodeBuilder {
 public:
  static ftl::RefPtr<const NodeBuilder> Create(uint8_t level,
                                               ObjectId object_id) {
    if (object_id.empty()) {
      return nullptr;
    }
    return AdoptRef(new ExistingNodeBuilder(level, std::move(object_id)));
  }

  // Build an |ExistingNodeBuilder| from |object_id|.
  static void FromId(
      PageStorage* page_storage,
      ObjectIdView object_id,
      std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback);

 private:
  // Extract the entries and children from a TreeNode.
  static void ExtractContent(
      const TreeNode& node,
      ftl::RefPtr<const RefVector<Entry>>* entries,
      ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>* children);

  ExistingNodeBuilder(
      uint8_t level,
      ObjectId object_id,
      ftl::RefPtr<const RefVector<Entry>> entries,
      ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>> children)
      : NodeBuilder(level),
        object_id_(std::move(object_id)),
        entries_(std::move(entries)),
        children_(std::move(children)) {}

  ExistingNodeBuilder(uint8_t level, ObjectId object_id)
      : ExistingNodeBuilder(level, std::move(object_id), {}, {}) {}

  // NodeBuilder:
  void Build(
      PageStorage* page_storage,
      std::function<void(Status,
                         std::pair<ObjectId, std::unordered_set<ObjectId>>)>
          callback) const override;
  void GetContent(
      PageStorage* page_storage,
      std::function<void(
          Status,
          const ftl::RefPtr<const RefVector<Entry>>&,
          const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&)>
          callback) const override;

  // The |ObjectId| of the existing node.
  ObjectId object_id_;

  // Cached values for the entries and children of the existing nodes.
  mutable ftl::RefPtr<const RefVector<Entry>> entries_;
  mutable ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>
      children_;
};

// A NodeBuilder that represents a new node that needs to be built.
class NewNodeBuilder : public NodeBuilder {
 public:
  static ftl::RefPtr<const NodeBuilder> Create(
      uint8_t level,
      ftl::RefPtr<const RefVector<Entry>> entries,
      ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>> children) {
    FTL_DCHECK(entries->size() + 1 == children->size());
    if (entries->empty() && !children->Get(0)) {
      return nullptr;
    }
    return AdoptRef(
        new NewNodeBuilder(level, std::move(entries), std::move(children)));
  }

  static ftl::RefPtr<const NodeBuilder> Create(
      uint8_t level,
      std::vector<Entry> entries,
      std::vector<ftl::RefPtr<const NodeBuilder>> children) {
    return Create(
        level, RefVector<Entry>::Create(std::move(entries)),
        RefVector<ftl::RefPtr<const NodeBuilder>>::Create(std::move(children)));
  }

 private:
  NewNodeBuilder(
      uint8_t level,
      ftl::RefPtr<const RefVector<Entry>> entries,
      ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>> children)
      : NodeBuilder(level),
        entries_(std::move(entries)),
        children_(std::move(children)) {
    FTL_DCHECK(!entries_->empty() || children_->Get(0));
  }

  // NodeBuilder:
  void Build(
      PageStorage* page_storage,
      std::function<void(Status,
                         std::pair<ObjectId, std::unordered_set<ObjectId>>)>
          callback) const override;
  void GetContent(
      PageStorage* page_storage,
      std::function<void(
          Status,
          const ftl::RefPtr<const RefVector<Entry>>&,
          const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&)>
          callback) const override;

  // The entries and children of the new node.
  ftl::RefPtr<const RefVector<Entry>> entries_;
  ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>> children_;
};

// Returns the index of |entries| that contains |key|, or the first entry that
// has key greather than |key|. In the second case, the key, if present, will be
// found in the children at the returned index.
size_t GetEntryOrChildIndex(const ftl::RefPtr<const RefVector<Entry>>& entries,
                            const std::string& key) {
  auto lower = std::lower_bound(entries->begin(), entries->end(), key,
                                [](const Entry& entry, const std::string& key) {
                                  return entry.key < key;
                                });
  return lower - entries->begin();
}

void NodeBuilder::Apply(
    ftl::RefPtr<const NodeBuilder> node_builder,
    const NodeLevelCalculator* node_level_calculator,
    PageStorage* page_storage,
    EntryChange change,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback) {
  if (node_builder) {
    node_builder->Apply(node_level_calculator, page_storage, std::move(change),
                        std::move(callback));
    return;
  }

  // If the change is a deletion, and the tree is null, the result is still
  // null.
  if (change.deleted) {
    callback(Status::OK, nullptr);
    return;
  }

  // Otherwise, create a node of the right level that contains only entry.
  uint8_t level = node_level_calculator->GetNodeLevel(change.entry.key);
  callback(Status::OK, NewNodeBuilder::Create(level, {std::move(change.entry)},
                                              {nullptr, nullptr}));
}

void NodeBuilder::Apply(
    const NodeLevelCalculator* node_level_calculator,
    PageStorage* page_storage,
    EntryChange change,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback)
    const {
  uint8_t change_level = node_level_calculator->GetNodeLevel(change.entry.key);

  if (change_level < level_) {
    // The change is at a lower level than the current node. Find the children
    // to apply the change, transform it and reconstruct the new node.
    GetContent(
        page_storage,
        [
          ref_this = ftl::RefPtr<const NodeBuilder>(this),
          node_level_calculator, page_storage, change = std::move(change),
          callback = std::move(callback)
        ](Status status, const ftl::RefPtr<const RefVector<Entry>>& entries,
          const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&
              children) {
          if (status != Status::OK) {
            callback(status, nullptr);
            return;
          }
          size_t index = GetEntryOrChildIndex(entries, change.entry.key);
          FTL_DCHECK(index == entries->size() ||
                     entries->Get(index).key != change.entry.key);

          ftl::RefPtr<const NodeBuilder> child = children->Get(index);

          // Apply the change recursively.
          Apply(child, node_level_calculator, page_storage, std::move(change), [
            ref_this, entries, children, index, child,
            callback = std::move(callback)
          ](Status status, ftl::RefPtr<const NodeBuilder> new_child) {
            if (status != Status::OK) {
              callback(status, nullptr);
              return;
            }

            // If the change is a no-op, just returns the original node.
            if (new_child == child) {
              callback(Status::OK, ref_this);
              return;
            }

            // Rebuild the list of children by replacing the child that the
            // change was applied on by the result of the change.
            auto lower = children->begin() + index;
            ftl::RefPtr<RefVector<ftl::RefPtr<const NodeBuilder>>>
                new_children = children->CopyUntil(lower);
            new_children->push_back(std::move(new_child));
            if (lower != children->end()) {
              new_children->Append(lower + 1, children->end());
            }
            callback(Status::OK, NewNodeBuilder::Create(
                                     ref_this->level_, std::move(entries),
                                     std::move(new_children)));
          });

        });
    return;
  }

  // Makes the callback asynchronous to not overflow the stack.
  auto asynchronous_callback = callback::MakeAsynchronous(std::move(callback));

  if (change.deleted) {
    Delete(page_storage, change_level, std::move(change.entry.key),
           std::move(asynchronous_callback));
    return;
  }

  Update(page_storage, change_level, std::move(change.entry),
         std::move(asynchronous_callback));
}

void NodeBuilder::Delete(
    PageStorage* page_storage,
    uint8_t key_level,
    std::string key,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback)
    const {
  FTL_DCHECK(key_level >= level_);

  auto ref_this = ftl::RefPtr<const NodeBuilder>(this);

  // If the change is at a higher level than this node, then it is a no-op.
  if (key_level > level_) {
    callback(Status::OK, std::move(ref_this));
    return;
  }

  GetContent(
      page_storage,
      [
        ref_this = std::move(ref_this), page_storage, key = std::move(key),
        callback = std::move(callback)
      ](Status status, const ftl::RefPtr<const RefVector<Entry>>& entries,
        const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&
            children) {
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        size_t index = GetEntryOrChildIndex(entries, key);

        // The key must be in the current node if it is in the tree.
        if (index == entries->size() || entries->Get(index).key != key) {
          // The key is not found. Return the current node.
          callback(status, std::move(ref_this));
          return;
        }

        // Element at |index| must be removed.
        Merge(page_storage, children->Get(index), children->Get(index + 1), [
          level = ref_this->level_, entries, children, index,
          callback = std::move(callback)
        ](Status status, ftl::RefPtr<const NodeBuilder> merged_child) {
          if (status != Status::OK) {
            callback(status, nullptr);
            return;
          }

          auto entries_split = entries->begin() + index;
          ftl::RefPtr<RefVector<Entry>> new_entries =
              entries->CopyUntil(entries_split);
          // Skip the deleted entry.
          ++entries_split;
          new_entries->Append(entries_split, entries->end());

          auto children_split = children->begin() + index;
          ftl::RefPtr<RefVector<ftl::RefPtr<const NodeBuilder>>> new_children =
              children->CopyUntil(children_split);
          new_children->push_back(merged_child);
          // Skip 2 children.
          children_split += 2;
          new_children->Append(children_split, children->end());

          callback(Status::OK,
                   NewNodeBuilder::Create(level, std::move(new_entries),
                                          std::move(new_children)));
        });
      });
}

void NodeBuilder::Update(
    PageStorage* page_storage,
    uint8_t change_level,
    Entry entry,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback)
    const {
  FTL_DCHECK(change_level >= level_);

  // If the change is at a greater level than the node level, the current node
  // must be splitted in 2, and the new root is composed of the new entry and
  // the 2 children.
  if (change_level > level_) {
    Split(
        page_storage, entry.key,
        [
          refcounted_this = ftl::RefPtr<const NodeBuilder>(this), change_level,
          entry = std::move(entry), callback = std::move(callback)
        ](Status status, ftl::RefPtr<const NodeBuilder> left,
          ftl::RefPtr<const NodeBuilder> right) {
          if (status != Status::OK) {
            callback(status, nullptr);
            return;
          }

          callback(Status::OK,
                   NewNodeBuilder::Create(change_level, {std::move(entry)},
                                          {ToLevel(left, change_level - 1),
                                           ToLevel(right, change_level - 1)}));
        });
    return;
  }

  GetContent(
      page_storage,
      [
        ref_this = ftl::RefPtr<const NodeBuilder>(this), page_storage,
        entry = std::move(entry), callback = std::move(callback)
      ](Status status, const ftl::RefPtr<const RefVector<Entry>>& entries,
        const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&
            children) {
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        // The change is at the current level. The entries must be splitted
        // according to the key of the change.
        size_t split_index = GetEntryOrChildIndex(entries, entry.key);

        if (split_index < entries->size() &&
            entries->Get(split_index).key == entry.key) {
          // The key is already present in the current entries of the node. The
          // value must be replaced.

          // Values is identical, the change is a no-op.
          if (entries->Get(split_index).object_id == entry.object_id) {
            callback(Status::OK, ref_this);
            return;
          }

          auto split_entry = entries->begin() + split_index;
          auto new_entries = entries->CopyUntil(split_entry);
          new_entries->push_back(std::move(entry));
          ++split_entry;
          new_entries->Append(split_entry, entries->end());
          callback(Status::OK, NewNodeBuilder::Create(ref_this->level_,
                                                      std::move(new_entries),
                                                      std::move(children)));
          return;
        }

        // Split the child that encompass |entry.key|.
        Split(children->Get(split_index), page_storage, entry.key,
              [
                level = ref_this->level_, entry = std::move(entry), entries,
                children, split_index, callback = std::move(callback)
              ](Status status, ftl::RefPtr<const NodeBuilder> left,
                ftl::RefPtr<const NodeBuilder> right) {
                if (status != Status::OK) {
                  callback(status, nullptr);
                  return;
                }

                // Add |entry| to the list of entries of the result node.
                auto split_entry = entries->begin() + split_index;
                auto new_entries = entries->CopyUntil(split_entry);
                new_entries->push_back(std::move(entry));
                new_entries->Append(split_entry, entries->end());

                auto split_child = children->begin() + split_index;
                auto new_children = children->CopyUntil(split_child);
                // Replace the child by the result of the split.
                new_children->push_back(left);
                new_children->push_back(right);
                ++split_child;
                new_children->Append(split_child, children->end());

                callback(Status::OK,
                         NewNodeBuilder::Create(level, std::move(new_entries),
                                                std::move(new_children)));
              });

      });
}

void NodeBuilder::Split(
    PageStorage* page_storage,
    std::string key,
    std::function<void(Status,
                       ftl::RefPtr<const NodeBuilder>,
                       ftl::RefPtr<const NodeBuilder>)> callback) const {
  GetContent(
      page_storage,
      [
        ref_this = ftl::RefPtr<const NodeBuilder>(this), page_storage,
        key = std::move(key), callback = std::move(callback)
      ](Status status, const ftl::RefPtr<const RefVector<Entry>>& entries,
        const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&
            children) {
        if (status != Status::OK) {
          callback(status, nullptr, nullptr);
          return;
        }

        // Find the index at which to split.
        size_t split_index = GetEntryOrChildIndex(entries, key);

        // Ensure that |key| is not part of the entries.
        FTL_DCHECK(split_index == entries->size() ||
                   entries->Get(split_index).key != key);

        auto child_to_split = children->Get(split_index);

        if (split_index == 0 && !child_to_split) {
          callback(Status::OK, nullptr, ref_this);
          return;
        }

        if (split_index == entries->size() && !child_to_split) {
          callback(Status::OK, ref_this, nullptr);
          return;
        }

        // Recursively call |Split| on the child.
        Split(child_to_split, page_storage, std::move(key),
              [
                ref_this, entries, children, split_index,
                callback = std::move(callback)
              ](Status status, ftl::RefPtr<const NodeBuilder> left,
                ftl::RefPtr<const NodeBuilder> right) {
                if (status != Status::OK) {
                  callback(status, nullptr, nullptr);
                  return;
                }

                auto entry_split = entries->begin() + split_index;
                auto children_split = children->begin() + split_index;

                ftl::RefPtr<RefVector<Entry>> left_entries =
                    entries->CopyUntil(entry_split);
                ftl::RefPtr<RefVector<Entry>> right_entries =
                    RefVector<Entry>::Create(entry_split, entries->end());

                ftl::RefPtr<RefVector<ftl::RefPtr<const NodeBuilder>>>
                    left_children = children->CopyUntil(children_split);
                left_children->push_back(left);
                // skip the splitted child.
                ++children_split;
                ftl::RefPtr<RefVector<ftl::RefPtr<const NodeBuilder>>>
                    right_children =
                        RefVector<ftl::RefPtr<const NodeBuilder>>::Create();
                right_children->push_back(right);
                right_children->Append(children_split, children->end());

                callback(Status::OK,
                         NewNodeBuilder::Create(ref_this->level_,
                                                std::move(left_entries),
                                                std::move(left_children)),
                         NewNodeBuilder::Create(ref_this->level_,
                                                std::move(right_entries),
                                                std::move(right_children)));
              });
      });
}

void NodeBuilder::Merge(
    PageStorage* page_storage,
    ftl::RefPtr<const NodeBuilder> other,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback)
    const {
  FTL_DCHECK(level_ == other->level_);

  GetContent(
      page_storage,
      [ level = level_, page_storage, other, callback = std::move(callback) ](
          Status status,
          const ftl::RefPtr<const RefVector<Entry>>& left_entries,
          const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&
              left_children) {
        if (status != Status::OK) {
          callback(status, nullptr);
          return;
        }

        other->GetContent(
            page_storage,
            [
              level, page_storage, left_entries, left_children,
              callback = std::move(callback)
            ](Status status,
              const ftl::RefPtr<const RefVector<Entry>>& right_entries,
              const ftl::RefPtr<const RefVector<
                  ftl::RefPtr<const NodeBuilder>>>& right_children) {
              if (status != Status::OK) {
                callback(status, nullptr);
                return;
              }

              // Merge the right-most child from |left| with the left-most child
              // from |right|.
              Merge(page_storage, left_children->back(),
                    right_children->front(),
                    [
                      level, left_entries, left_children, right_entries,
                      right_children, callback = std::move(callback)
                    ](Status status,
                      ftl::RefPtr<const NodeBuilder> merged_child) {
                      if (status != Status::OK) {
                        callback(status, nullptr);
                        return;
                      }

                      // Concatenate entries.
                      auto new_entries =
                          left_entries->CopyUntil(left_entries->end());
                      new_entries->Append(right_entries->begin(),
                                          right_entries->end());

                      // Concatenate children replacing the  right-most child
                      // from |left| and the  left-most child from |right| with
                      // the merged child.
                      auto new_children =
                          left_children->CopyUntil(left_children->end() - 1);
                      new_children->push_back(merged_child);
                      new_children->Append(right_children->begin() + 1,
                                           right_children->end());

                      callback(Status::OK, NewNodeBuilder::Create(
                                               level, std::move(new_entries),
                                               std::move(new_children)));
                    });
            });
      });
}

ftl::RefPtr<const NodeBuilder> NodeBuilder::ToLevel(
    uint8_t target_level) const {
  FTL_DCHECK(target_level >= level_);

  ftl::RefPtr<const NodeBuilder> result(this);
  size_t current_level = level_;
  while (current_level < target_level) {
    ++current_level;
    result =
        NewNodeBuilder::Create(current_level, std::vector<Entry>(), {result});
  }
  return result;
}

void ExistingNodeBuilder::FromId(
    PageStorage* page_storage,
    ObjectIdView object_id,
    std::function<void(Status, ftl::RefPtr<const NodeBuilder>)> callback) {
  TreeNode::FromId(page_storage, object_id, [
    object_id = object_id.ToString(), callback = std::move(callback)
  ](Status status, std::unique_ptr<const TreeNode> node) {
    if (status != Status::OK) {
      callback(status, nullptr);
      return;
    }

    FTL_DCHECK(node);

    ftl::RefPtr<const RefVector<Entry>> entries;
    ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>> children;
    ExtractContent(*node, &entries, &children);
    callback(Status::OK, ftl::AdoptRef(new ExistingNodeBuilder(
                             node->level(), std::move(object_id),
                             std::move(entries), std::move(children))));
  });
}

void ExistingNodeBuilder::Build(
    PageStorage* page_storage,
    std::function<void(Status,
                       std::pair<ObjectId, std::unordered_set<ObjectId>>)>
        callback) const {
  callback(Status::OK, std::make_pair(std::move(object_id_),
                                      std::unordered_set<ObjectId>()));
}

void ExistingNodeBuilder::GetContent(
    PageStorage* page_storage,
    std::function<void(
        Status,
        const ftl::RefPtr<const RefVector<Entry>>&,
        const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&)>
        callback) const {
  FTL_DCHECK(object_id_ != "");
  if (children_) {
    callback(Status::OK, entries_, children_);
    return;
  }
  TreeNode::FromId(page_storage, object_id_, [
    ref_this = ftl::RefPtr<const ExistingNodeBuilder>(this),
    callback = std::move(callback)
  ](Status status, std::unique_ptr<const TreeNode> node) {
    if (status != Status::OK) {
      callback(status, {}, {});
      return;
    }
    FTL_DCHECK(node);

    ExtractContent(*node, &ref_this->entries_, &ref_this->children_);
    callback(Status::OK, ref_this->entries_, ref_this->children_);
  });
}

void NewNodeBuilder::Build(
    PageStorage* page_storage,
    std::function<void(Status,
                       std::pair<ObjectId, std::unordered_set<ObjectId>>)>
        callback) const {
  auto asynchronous_callback = callback::MakeAsynchronous(std::move(callback));

  // Build all children.
  auto waiter = callback::
      Waiter<Status, std::pair<ObjectId, std::unordered_set<ObjectId>>>::Create(
          Status::OK);
  for (auto& child : *children_) {
    NodeBuilder::Build(child, page_storage, waiter->NewCallback());
  }

  waiter->Finalize([
    ref_this = ftl::RefPtr<const NewNodeBuilder>(this), page_storage,
    callback = std::move(asynchronous_callback)
  ](Status status,
    std::vector<std::pair<ObjectId, std::unordered_set<ObjectId>>>
        children) mutable {
    if (status != Status::OK) {
      callback(status, std::pair<ObjectId, std::unordered_set<ObjectId>>());
      return;
    }

    std::vector<ObjectId> children_ids;
    std::unordered_set<ObjectId> new_ids;
    for (auto& pair : children) {
      children_ids.push_back(std::move(pair.first));
      new_ids.insert(pair.second.begin(), pair.second.end());
    }
    TreeNode::FromEntries(
        page_storage, ref_this->level_,
        std::vector<Entry>(ref_this->entries_->begin(),
                           ref_this->entries_->end()),
        children_ids,
        [ new_ids = std::move(new_ids), callback = std::move(callback) ](
            Status status, ObjectId object_id) mutable {
          new_ids.insert(object_id);
          callback(status,
                   std::make_pair(std::move(object_id), std::move(new_ids)));
        });
  });
}

void NewNodeBuilder::GetContent(
    PageStorage* page_storage,
    std::function<void(
        Status,
        const ftl::RefPtr<const RefVector<Entry>>&,
        const ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>&)>
        callback) const {
  callback(Status::OK, entries_, children_);
}

void ExistingNodeBuilder::ExtractContent(
    const TreeNode& node,
    ftl::RefPtr<const RefVector<Entry>>* entries,
    ftl::RefPtr<const RefVector<ftl::RefPtr<const NodeBuilder>>>* children) {
  FTL_DCHECK(entries);
  FTL_DCHECK(children);
  *entries =
      RefVector<Entry>::Create(node.entries().begin(), node.entries().end());
  auto mutable_children = RefVector<ftl::RefPtr<const NodeBuilder>>::Create();
  for (const auto& child_id : node.children_ids()) {
    mutable_children->push_back(
        ExistingNodeBuilder::Create(node.level() - 1, child_id));
  }
  *children = mutable_children;
}

// Apply |changes| on |root|. This is called recursively until |changes| is not
// valid anymore. At this point, build is called on |root|.
void ApplyChangesOnRoot(
    const NodeLevelCalculator* node_level_calculator,
    PageStorage* page_storage,
    ftl::RefPtr<const NodeBuilder> root,
    std::unique_ptr<Iterator<const EntryChange>> changes,
    std::function<void(Status, ObjectId, std::unordered_set<ObjectId>)>
        callback) {
  if (!changes->Valid()) {
    if (changes->GetStatus() != Status::OK) {
      callback(changes->GetStatus(), "", {});
      return;
    }

    NodeBuilder::Build(
        root, page_storage,
        [callback = std::move(callback)](
            Status status,
            std::pair<ObjectId, std::unordered_set<ObjectId>> result) {
          callback(status, std::move(result.first), std::move(result.second));
        });
    return;
  }
  EntryChange change = std::move(**changes);
  changes->Next();
  NodeBuilder::Apply(
      root, node_level_calculator, page_storage, std::move(change),
      ftl::MakeCopyable([
        node_level_calculator, page_storage, changes = std::move(changes),
        callback = std::move(callback)
      ](Status status, ftl::RefPtr<const NodeBuilder> new_root) mutable {
        if (status != Status::OK) {
          callback(status, "", {});
          return;
        }
        ApplyChangesOnRoot(node_level_calculator, page_storage, new_root,
                           std::move(changes), std::move(callback));
      }));
}

}  // namespace

const NodeLevelCalculator* GetDefaultNodeLevelCalculator() {
  return &kDefaultNodeLevelCalculator;
}

void ApplyChanges(
    PageStorage* page_storage,
    ObjectIdView root_id,
    std::unique_ptr<Iterator<const EntryChange>> changes,
    std::function<void(Status, ObjectId, std::unordered_set<ObjectId>)>
        callback,
    const NodeLevelCalculator* node_level_calculator) {
  ExistingNodeBuilder::FromId(
      page_storage, root_id.ToString(), ftl::MakeCopyable([
        node_level_calculator, page_storage, changes = std::move(changes),
        callback = std::move(callback)
      ](Status status, ftl::RefPtr<const NodeBuilder> root) mutable {
        if (status != Status::OK) {
          callback(status, "", {});
          return;
        }
        ApplyChangesOnRoot(
            node_level_calculator, page_storage, root, std::move(changes),
            [ page_storage, callback = std::move(callback) ](
                Status status, ObjectId object_id,
                std::unordered_set<ObjectId> new_ids) {
              if (status != Status::OK || !object_id.empty()) {
                callback(status, std::move(object_id), std::move(new_ids));
                return;
              }
              TreeNode::Empty(
                  page_storage, [callback = std::move(callback)](
                                    Status status, ObjectId object_id) {
                    std::unordered_set<ObjectId> new_ids({object_id});
                    callback(status, std::move(object_id), std::move(new_ids));
                  });
            });
      }));
}

void GetObjectIds(PageStorage* page_storage,
                  ObjectIdView root_id,
                  std::function<void(Status, std::set<ObjectId>)> callback) {
  FTL_DCHECK(!root_id.empty());
  auto object_ids = std::make_unique<std::set<ObjectId>>();
  object_ids->insert(root_id.ToString());

  auto on_next = [object_ids = object_ids.get()](EntryAndNodeId e) {
    object_ids->insert(e.entry.object_id);
    object_ids->insert(e.node_id);
    return true;
  };
  auto on_done = ftl::MakeCopyable([
    object_ids = std::move(object_ids), callback = std::move(callback)
  ](Status status) {
    if (status != Status::OK) {
      callback(status, std::set<ObjectId>());
      return;
    }
    callback(status, std::move(*object_ids));
  });
  ForEachEntry(page_storage, root_id, "", std::move(on_next),
               std::move(on_done));
}

void GetObjectsFromSync(ObjectIdView root_id,
                        PageStorage* page_storage,
                        std::function<void(Status)> callback) {
  ftl::RefPtr<callback::Waiter<Status, std::unique_ptr<const Object>>> waiter_ =
      callback::Waiter<Status, std::unique_ptr<const Object>>::Create(
          Status::OK);
  auto on_next = [page_storage, waiter_](EntryAndNodeId e) {
    if (e.entry.priority == KeyPriority::EAGER) {
      page_storage->GetObject(e.entry.object_id, PageStorage::Location::NETWORK,
                              waiter_->NewCallback());
    }
    return true;
  };
  auto on_done = [ callback = std::move(callback), waiter_ ](Status status) {
    if (status != Status::OK) {
      callback(status);
      return;
    }
    waiter_->Finalize([callback = std::move(callback)](
        Status s, std::vector<std::unique_ptr<const Object>> objects) {
      callback(s);
    });
  };
  ForEachEntry(page_storage, root_id, "", std::move(on_next),
               std::move(on_done));
}

void ForEachEntry(PageStorage* page_storage,
                  ObjectIdView root_id,
                  std::string min_key,
                  std::function<bool(EntryAndNodeId)> on_next,
                  std::function<void(Status)> on_done) {
  FTL_DCHECK(!root_id.empty());
  TreeNode::FromId(page_storage, root_id, [
    min_key = std::move(min_key), page_storage, on_next = std::move(on_next),
    on_done = std::move(on_done)
  ](Status status, std::unique_ptr<const TreeNode> root) {
    if (status != Status::OK) {
      on_done(status);
      return;
    }
    ForEachEntryInSubtree(
        page_storage, std::move(root), std::move(min_key),
        std::move(on_next), [on_done = std::move(on_done)](Status s, bool) {
          on_done(s);
        });

  });
}

void ForEachDiff(PageStorage* page_storage,
                 ObjectIdView base_root_id,
                 ObjectIdView other_root_id,
                 std::function<bool(EntryChange)> on_next,
                 std::function<void(Status)> on_done) {
  // TODO(nellyv): This is a naive calculation of the the diff, loading all
  // entries from both versions in memory and then computing the diff. This
  // should be updated with the new version of the BTree.
  auto waiter =
      callback::Waiter<Status, std::unique_ptr<std::vector<Entry>>>::Create(
          Status::OK);
  GetEntriesVector(page_storage, base_root_id, waiter->NewCallback());
  GetEntriesVector(page_storage, other_root_id, waiter->NewCallback());
  waiter->Finalize([
    on_next = std::move(on_next), on_done = std::move(on_done)
  ](Status s, std::vector<std::unique_ptr<std::vector<Entry>>> entries) {
    if (s != Status::OK) {
      on_done(s);
      return;
    }
    FTL_DCHECK(entries.size() == 2u);
    auto base_it = entries[0].get()->begin();
    auto base_it_end = entries[0].get()->end();
    auto other_it = entries[1].get()->begin();
    auto other_it_end = entries[1].get()->end();

    while (base_it != base_it_end && other_it != other_it_end) {
      if (*base_it == *other_it) {
        // Entries are equal.
        ++base_it;
        ++other_it;
        continue;
      }
      EntryChange change;
      // strcmp will not work if keys contain '\0' characters.
      int cmp = ftl::StringView(base_it->key).compare(other_it->key);
      if (cmp >= 0) {
        // The entry was added or updated.
        change = {*other_it, false};
      } else {
        // The entry was deleted.
        change = {*base_it, true};
      }
      if (!on_next(std::move(change))) {
        on_done(Status::OK);
        return;
      }
      // Advance the iterators.
      if (cmp >= 0) {
        ++other_it;
      }
      if (cmp <= 0) {
        ++base_it;
      }
    }
    while (base_it != base_it_end) {
      // The entry was deleted.
      EntryChange change{*base_it, true};
      if (!on_next(std::move(change))) {
        on_done(Status::OK);
        return;
      }
      base_it++;
    }
    while (other_it != other_it_end) {
      // The entry was added.
      EntryChange change{*other_it, false};
      if (!on_next(std::move(change))) {
        on_done(Status::OK);
        return;
      }
      other_it++;
    }
    on_done(Status::OK);
  });
}

}  // namespace btree
}  // namespace storage
