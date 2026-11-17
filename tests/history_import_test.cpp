#include "../HumiditySensor/HistoryImport.h"
#include <assert.h>
#include <stdio.h>

int main() {
  struct Packet { char magic[4]; uint32_t count; struct { uint32_t age; float h,t; } points[2]; };
  Packet packet = {{'H','S','T','1'}, 2, {{300000,45,20},{0,NAN,NAN}}};
  HistoryImport parser;
  const auto bytes = reinterpret_cast<const uint8_t*>(&packet);
  parser.begin(1000);
  for (unsigned i=0;i<sizeof(packet);++i) parser.feed(bytes+i,1);
  assert(parser.complete() && parser.staged().size()==2);
  assert(uint32_t(1000-parser.staged().at(0).sampledAt)==300000);
  assert(isnan(parser.staged().at(1).humidity));
  parser.begin(1000);parser.feed(bytes,sizeof(packet)-1);assert(!parser.complete());
  packet.count=290;parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(!parser.complete());
  packet.count=2;packet.points[1].age=300001;
  parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(!parser.complete());
  packet.points[1].age=0;packet.points[1].h=101;
  parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(!parser.complete());
  packet.points[1].h=INFINITY;
  parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(!parser.complete());
  packet.points[1].h=40;packet.points[0].age=RecentHistory::WINDOW_MS+1;
  parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(!parser.complete());
  packet.points[0].age=300000;
  parser.begin(1000);parser.feed(bytes,sizeof(packet));
  const uint8_t extra=0;parser.feed(&extra,1);assert(!parser.complete());
  parser.begin(1000);parser.feed(bytes,sizeof(packet));assert(parser.complete());
  uint8_t full[8 + RecentHistory::CAPACITY * 12] = {};
  memcpy(full, "HST1", 4);
  const uint32_t count = RecentHistory::CAPACITY;
  memcpy(full + 4, &count, 4);
  for (uint32_t i=0; i<count; ++i) {
    const uint32_t age=(count-1-i)*RecentHistory::INTERVAL_MS;
    const float humidity=45, temperature=20;
    memcpy(full+8+i*12,&age,4);memcpy(full+12+i*12,&humidity,4);memcpy(full+16+i*12,&temperature,4);
  }
  parser.begin(1000);
  for (size_t offset=0;offset<sizeof(full);offset+=37) {
    const size_t remaining=sizeof(full)-offset;
    parser.feed(full+offset,remaining<37?remaining:37);
  }
  assert(parser.complete() && parser.staged().size()==RecentHistory::CAPACITY);
  assert(uint32_t(1000-parser.staged().at(0).sampledAt)==RecentHistory::WINDOW_MS);
  assert(sizeof(HistoryImport)<=4096 && sizeof(RecentHistory)<=4096);
  puts("PASS: bounded streamed history import, gaps, age rollover, malformed/oversized/truncated data rejection");
}
