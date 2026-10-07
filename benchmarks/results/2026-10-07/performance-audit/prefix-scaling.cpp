#include "Document.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace downstream;
volatile size_t sink;
int main() {
  for (size_t count : {size_t(1024),size_t(2048),size_t(4096),size_t(8192)}) {
    std::vector<double> times;
    for (int sample=0;sample<9;sample++) {
      auto start=std::chrono::steady_clock::now();
      std::shared_ptr<const SharedList<size_t>::Prefix> prefix;
      for (size_t i=0;i<count;i++) prefix=SharedList<size_t>::Prefix::extend(prefix, {i});
      sink=(*prefix)[count-1];
      times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    std::sort(times.begin(),times.end());
    std::printf("prefix_extend count=%zu median_ms=%.6f first=%.6f last=%.6f\n",count,times[4],times.front(),times.back());
  }
  for (size_t units : {size_t(16384),size_t(32768),size_t(65536),size_t(131072)}) {
    Document doc;
    Block block;
    block.text.assign(units,u'x');
    for (size_t at=0;at<units;at+=8) block.tokens.push_back({uint32_t(at),uint32_t(at+4),TokenClass::Keyword});
    doc.blocks.push_back(std::move(block));
    std::vector<double> times;
    for(int sample=0;sample<9;sample++) {
      auto start=std::chrono::steady_clock::now();
      for(size_t i=0;i<1000;i++) { auto copy=std::make_shared<Document>(doc); sink=copy->blocks.back().tokens.size(); }
      times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    std::sort(times.begin(),times.end());
    std::printf("snapshot_copy units=%zu tokens=%zu repeats=1000 median_ms=%.6f\n",units,doc.blocks.back().tokens.size(),times[4]);
  }
}
