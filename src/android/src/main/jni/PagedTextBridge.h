#pragma once

#include "SnapshotText.h"

#include <jni.h>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace downstream {

/// Java strings contain immutable pages, shared across retained snapshots. Both
/// sides are weak: the bridge cannot prolong native documents or Java strings.
/// A live native owner also disambiguates allocator address reuse.
class PagedTextBridge {
  struct Key {
    const char16_t *data;
    size_t size;
    bool operator==(const Key &other) const { return data == other.data && size == other.size; }
  };
  struct Hash {
    size_t operator()(const Key &key) const {
      return std::hash<const char16_t *>{}(key.data) ^ (key.size * 31);
    }
  };
  struct Entry {
    Key key;
    std::weak_ptr<const void> owner;
    jweak string;
  };
  std::mutex mutex_;
  std::list<Entry> entries_;
  std::unordered_map<Key, std::list<Entry>::iterator, Hash> index_;
  size_t units_ = 0;
  uint64_t transferredUnits_ = 0;
  uint64_t reusedPages_ = 0;
  // Count and character budget both bound native bookkeeping and weak handles.
  static constexpr size_t kMaxPages = 2048;
  static constexpr size_t kMaxUnits = 2 * 1024 * 1024;

  void erase(JNIEnv *env, std::list<Entry>::iterator entry) {
    units_ -= entry->key.size;
    index_.erase(entry->key);
    env->DeleteWeakGlobalRef(entry->string);
    entries_.erase(entry);
  }

public:
  struct Statistics { uint64_t transferredUnits; uint64_t reusedPages; size_t cachedPages; };

  Statistics statistics() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {transferredUnits_, reusedPages_, entries_.size()};
  }

  void clear(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : entries_) env->DeleteWeakGlobalRef(entry.string);
    entries_.clear();
    index_.clear();
    units_ = 0;
    transferredUnits_ = 0;
    reusedPages_ = 0;
  }

  jstring page(JNIEnv *env, std::u16string_view text, std::shared_ptr<const void> owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owner) {
      auto value = env->NewString(reinterpret_cast<const jchar *>(text.data()), static_cast<jsize>(text.size()));
      if (value) transferredUnits_ += text.size();
      return value;
    }
    const Key key{text.data(), text.size()};
    const auto found = index_.find(key);
    if (found != index_.end()) {
      const auto entry = found->second;
      const auto previousOwner = entry->owner.lock();
      if (previousOwner && previousOwner.get() == owner.get()) {
        auto value = static_cast<jstring>(env->NewLocalRef(entry->string));
        if (value) {
          reusedPages_++;
          entries_.splice(entries_.end(), entries_, entry);
          return value;
        }
        if (env->ExceptionCheck()) return nullptr;
      }
      erase(env, entry);
    }
    auto value = env->NewString(reinterpret_cast<const jchar *>(text.data()), static_cast<jsize>(text.size()));
    if (!value) return nullptr;
    transferredUnits_ += text.size();
    auto weak = env->NewWeakGlobalRef(value);
    if (!weak) {
      if (env->ExceptionCheck()) { env->DeleteLocalRef(value); return nullptr; }
      return value;
    }
    while (!entries_.empty() && (entries_.size() >= kMaxPages || units_ + text.size() > kMaxUnits)) {
      erase(env, entries_.begin());
    }
    entries_.push_back({key, owner, weak});
    index_.emplace(key, std::prev(entries_.end()));
    units_ += text.size();
    return value;
  }

  jobjectArray text(JNIEnv *env, const SnapshotText &text) {
    jclass stringClass = env->FindClass("java/lang/String");
    if (!stringClass) return nullptr;
    auto result = env->NewObjectArray(static_cast<jsize>(text.chunkCount()), stringClass, nullptr);
    env->DeleteLocalRef(stringClass);
    if (!result) return nullptr;
    for (size_t index = 0; index < text.chunkCount(); index++) {
      auto value = page(env, text.chunk(index), text.hasPages() ? text.chunkOwner(index) : nullptr);
      if (!value) { env->DeleteLocalRef(result); return nullptr; }
      env->SetObjectArrayElement(result, static_cast<jsize>(index), value);
      env->DeleteLocalRef(value);
      if (env->ExceptionCheck()) { env->DeleteLocalRef(result); return nullptr; }
    }
    return result;
  }
};

} // namespace downstream
