// Exercises production JNI page caching and SnapshotText on the Android VM.
#include "PagedTextBridge.h"
#include <unordered_map>

using namespace downstream;
namespace {
PagedTextBridge bridge;
std::mutex snapshotsMutex;
std::unordered_map<jlong, SnapshotText> snapshots;
jlong nextId = 1;
std::u16string input(JNIEnv *env, jstring text) {
  const auto length = env->GetStringLength(text);
  std::u16string result(static_cast<size_t>(length), u'\0');
  env->GetStringRegion(text, 0, length, reinterpret_cast<jchar *>(result.data()));
  return result;
}
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_downstream_NativePages_create(JNIEnv *env, jclass, jstring text) {
  SnapshotText snapshot;
  snapshot.appendSnapshot(input(env, text));
  std::lock_guard<std::mutex> lock(snapshotsMutex);
  const auto id = nextId++;
  snapshots.emplace(id, std::move(snapshot));
  return id;
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_downstream_NativePages_createBuilder(JNIEnv *env, jclass, jstring text) {
  SnapshotText snapshot(input(env, text));
  std::lock_guard<std::mutex> lock(snapshotsMutex);
  const auto id = nextId++;
  snapshots.emplace(id, std::move(snapshot));
  return id;
}
extern "C" JNIEXPORT jlong JNICALL
Java_com_downstream_NativePages_append(JNIEnv *env, jclass, jlong id, jstring text) {
  const auto added = input(env, text);
  std::lock_guard<std::mutex> lock(snapshotsMutex);
  auto snapshot = snapshots.at(id);
  snapshot.appendSnapshot(added);
  const auto next = nextId++;
  snapshots.emplace(next, std::move(snapshot));
  return next;
}
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_downstream_NativePages_read(JNIEnv *env, jclass, jlong id) {
  SnapshotText snapshot;
  {
    std::lock_guard<std::mutex> lock(snapshotsMutex);
    snapshot = snapshots.at(id);
  }
  return bridge.text(env, snapshot);
}
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_NativePages_clearCache(JNIEnv *env, jclass) { bridge.clear(env); }
extern "C" JNIEXPORT void JNICALL
Java_com_downstream_NativePages_reset(JNIEnv *env, jclass) {
  bridge.clear(env);
  std::lock_guard<std::mutex> lock(snapshotsMutex);
  snapshots.clear();
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_downstream_NativePages_stats(JNIEnv *env, jclass) {
  const auto stats = bridge.statistics();
  uint64_t flattened = 0;
  {
    std::lock_guard<std::mutex> lock(snapshotsMutex);
    for (const auto &entry : snapshots) flattened += entry.second.flattenedBytes();
  }
  const jlong values[]{static_cast<jlong>(stats.transferredUnits), static_cast<jlong>(stats.reusedPages),
    static_cast<jlong>(stats.cachedPages), static_cast<jlong>(flattened)};
  auto result = env->NewLongArray(4);
  env->SetLongArrayRegion(result, 0, 4, values);
  return result;
}
