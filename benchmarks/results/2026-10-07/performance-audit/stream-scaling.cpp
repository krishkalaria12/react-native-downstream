#include "IncrementalParser.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace downstream;
int main(){
 for(bool colored:{false,true}) for(size_t bytes:{size_t(8192),size_t(16384),size_t(32768),size_t(61440)}) {
  std::string code;
  while(code.size()<bytes)code+="const value = compute(user.id, 42); // next value\n";
  code.resize(bytes);
  std::vector<double> samples;
  IncrementalCounters counters;
  for(int sample=0;sample<8;sample++){
   auto start=std::chrono::steady_clock::now();
   IncrementalParser parser(ParseMode::IncrementalTail);
   parser.append(colored?"```ts\n":"```\n"); parser.parse(false);
   for(size_t at=0;at<code.size();at+=16){parser.append(std::string_view(code).substr(at,16));parser.parse(false);}
   auto doc=parser.document();
   if(!doc || doc->blocks.back().text.size()!=bytes){std::printf("bad fence result\n");return 1;}
   counters=parser.counters();
   auto duration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(sample)samples.push_back(duration);
  }
  std::sort(samples.begin(),samples.end());
  std::printf("fence colored=%d bytes=%zu chunk=16 median_ms=%.6f parsed_bytes=%llu fast_updates=%llu\n",colored,bytes,samples[3],(unsigned long long)counters.bytesParsed,(unsigned long long)counters.openFenceFastUpdates);
 }
 for(bool reference:{false,true})for(size_t bytes:{size_t(16384),size_t(32768),size_t(65536),size_t(98304)}) {
  std::string source=reference?"[ref]\n\n":"plain\n\n";
  while(source.size()<bytes)source+="Normal paragraph text with **strong** wording.\n\n";
  source.resize(bytes);
  std::vector<double> samples;
  IncrementalCounters counters;
  size_t committed=0;
  for(int sample=0;sample<4;sample++){
   auto start=std::chrono::steady_clock::now();
   IncrementalParser parser(ParseMode::IncrementalTail);
   for(size_t at=0;at<source.size();at+=128){parser.append(std::string_view(source).substr(at,128));parser.parse(false);}
   counters=parser.counters();committed=parser.committedBytes();
   auto duration=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(sample)samples.push_back(duration);
  }
  std::sort(samples.begin(),samples.end());
  std::printf("prose reference=%d bytes=%zu chunk=128 median_ms=%.6f parsed_bytes=%llu full_parses=%llu tail_parses=%llu committed=%zu tail_limit=%llu\n",reference,bytes,samples[1],(unsigned long long)counters.bytesParsed,(unsigned long long)counters.fullParses,(unsigned long long)counters.tailParses,committed,(unsigned long long)counters.fallbackCount(ParseFallback::TailLimit));
 }
}
