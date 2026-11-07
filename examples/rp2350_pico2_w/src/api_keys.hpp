#pragma once
#include "storage.hpp"
#include "../../common/demo_api_keys.hpp"
#if NAWSERVER_DEMO_API_KEYS
#include "pico/rand.h"
#if !HAS_RP2350_TRNG || !PICO_RAND_ENTROPY_SRC_TRNG
#error "API keys require RP2350 TRNG entropy"
#endif
namespace rp_demo
{
class KeyStorage final : public NaWServer::keys::Storage
{
 static const char *path(unsigned record){return record?"/web.api-keys.1":"/web.api-keys.0";}
public:
 NaWServer::keys::ReadStatus read(unsigned record,void *bytes,std::size_t capacity,std::size_t &size)override
 {
  if(record>1)return NaWServer::keys::ReadStatus::Error;
  return storage().readKeyRecord(path(record),bytes,capacity,size);
 }
 bool write(unsigned record,const void *bytes,std::size_t size)override{return record<2&&storage().writeFile(path(record),bytes,size);}
};
class KeyRandom final : public NaWServer::keys::Random
{
public:
 bool fill(std::uint8_t *bytes,std::size_t size)override{while(size){const auto word=get_rand_32();const auto count=size<sizeof(word)?size:sizeof(word);std::memcpy(bytes,&word,count);bytes+=count;size-=count;}return true;}
};
inline KeyStorage key_storage;
inline KeyRandom key_random;
inline NaWServer::keys::Store key_store(key_storage,key_random);
inline nawserver_demo_keys::Service api_keys(key_store);
inline nawserver_demo_keys::Console key_console(api_keys);
}

#endif
