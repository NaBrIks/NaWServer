// SPDX-License-Identifier: MIT
#pragma once
#include <NaWServer/api_keys.h>
#include <NaWServer/admission_http.h>
#include <NaWServer/client_identity.h>
#include <NaWServer/request.h>
#include <NaWServer/detail/synchronization.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <charconv>

#include "demo_key_config.hpp"
#if NAWSERVER_DEMO_API_KEYS
namespace nawserver_demo_keys
{
static_assert(NAWSERVER_DEMO_SHARED_CLIENTS > 0, "Demo requires a shared page slot");
static_assert(NAWSERVER_DEMO_HTTP_RESERVED_REQUESTS >= NAWSERVER_API_KEYS_MAX + NAWSERVER_DEMO_BOOTSTRAP_HTTP, "Physical reserve must cover protected keys and bootstrap");
static_assert(NAWSERVER_DEMO_HTTP_INITIAL_REQUESTS >= NAWSERVER_DEMO_HTTP_RESERVED_REQUESTS, "Initial HTTP buffers must include the reserve");
static_assert(NAWSERVER_DEMO_HTTP_MAXIMUM_REQUESTS > NAWSERVER_DEMO_HTTP_RESERVED_REQUESTS && NAWSERVER_DEMO_HTTP_MAXIMUM_REQUESTS >= NAWSERVER_DEMO_HTTP_INITIAL_REQUESTS, "HTTP pool must also admit ordinary pages");
static_assert(NAWSERVER_ADMISSION_MAX_ACTIVE >= NAWSERVER_API_KEYS_MAX + NAWSERVER_DEMO_BOOTSTRAP_HTTP + NAWSERVER_DEMO_SHARED_CLIENTS * (NAWSERVER_DEMO_SHARED_HTTP + 2), "Admission permit budget is too small for demo profiles");
class Service
{
 NaWServer::keys::Store &store_;
 NaWServer::detail::Mutex mutex_;
 NaWServer::ClientIdentity pages_{NaWServer::ClientIdentitySource::Page};
 class Writer
 {
  std::array<std::uint8_t,1536> bytes_{}; std::size_t size_=0;
 public:
  void byte(unsigned v){if(size_<bytes_.size())bytes_[size_++]=static_cast<std::uint8_t>(v);}
  void number(unsigned type,std::size_t v){if(v<24)byte(type*32+v);else if(v<256){byte(type*32+24);byte(v);}else{byte(type*32+25);byte(v>>8);byte(v);}}
  void text(std::string_view v){number(3,v.size());for(auto c:v)byte(static_cast<unsigned char>(c));}
  void field(const char *key,std::string_view v){text(key);text(v);}
  void field(const char *key,std::size_t v){text(key);number(0,v);}
  void clear(){bytes_.fill(0);size_=0;}
  NaWServer::ByteView view()const{return {bytes_.data(),size_};}
 };
 Writer output_;
 static bool readCommand(NaWServer::ByteView body,std::string_view &action,std::string_view &name,std::size_t &slot)
 {
  if(body.empty()||body.data()[0]<0xa1||body.data()[0]>0xa3)return false;
  const auto *p=body.data()+1,*end=body.data()+body.size(); unsigned seen=0;
  auto text=[&](std::string_view &v){if(p==end||(*p>>5)!=3)return false;std::size_t n=*p++&31;if(n==24){if(p==end)return false;n=*p++;}else if(n>24)return false;if(n>std::size_t(end-p))return false;v={reinterpret_cast<const char*>(p),n};p+=n;return true;};
  for(unsigned i=0;i<(body.data()[0]&31);++i){std::string_view key,value;if(!text(key))return false;unsigned bit=key=="action"?1:key=="name"?2:key=="slot"?4:0;if(!bit||(seen&bit))return false;seen|=bit;if(bit==4){if(p==end||*p>=24)return false;slot=*p++;}else{if(!text(value))return false;if(bit==1)action=value;else name=value;}}
  return p==end&&(seen&1);
 }
public:
 explicit Service(NaWServer::keys::Store &store):store_(store){}
 bool initialize(){NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);return store_.load()==NaWServer::keys::Status::Ok;}
 static NaWServer::HeaderReadResult resolve(void *opaque,const NaWServer::HeaderContext &context,char *dest,std::size_t capacity)
 {
  auto &self=*static_cast<Service*>(opaque); char secret[65]{};
  auto result=context.request.readHeader("X-Client-Key",secret,sizeof(secret));
  if(result.status!=NaWServer::HeaderReadStatus::NotFound){if(result.status!=NaWServer::HeaderReadStatus::Ok)return result;NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(self.mutex_);const auto slot=self.store_.authenticate({secret,result.size});std::memset(secret,0,sizeof(secret));if(!slot)return {NaWServer::HeaderReadStatus::InvalidArgument,0};const auto id=NaWServer::keys::Store::identity(*slot);if(capacity<=id.size())return {NaWServer::HeaderReadStatus::BufferTooSmall,id.size()};std::memcpy(dest,id.data(),id.size());dest[id.size()]=0;return {NaWServer::HeaderReadStatus::Ok,id.size()};}
    const auto page=self.pages_.resolve(context,dest,capacity);
  if(page.status!=NaWServer::HeaderReadStatus::Ok)return page;
  if(std::string_view(dest,page.size).find(":p")==std::string_view::npos){constexpr std::string_view bootstrap="naw-bootstrap";if(capacity<=bootstrap.size())return {NaWServer::HeaderReadStatus::BufferTooSmall,bootstrap.size()};std::memcpy(dest,bootstrap.data(),bootstrap.size());dest[bootstrap.size()]=0;return {NaWServer::HeaderReadStatus::Ok,bootstrap.size()};}
  return page;
 }
 NaWServer::Result http(NaWServer::Context &context)
 {
  using namespace NaWServer;
  auto &request=context.request;auto &response=context.response;
  if(response.header("Cache-Control","no-store")!=SendResult::Ok)return Result::Error;
  if(const auto origin=request.header("Origin");origin&&*origin!="http://"+request.host())return responseResult(response.text(403,"Origin rejected"));
  detail::LockGuard<detail::Mutex> lock(mutex_);if(!store_.ready())return responseResult(response.text(503,"Key store unavailable"));auto &out=output_;out.clear();struct ClearOutput{Writer &writer;~ClearOutput(){writer.clear();}} clear_output{out};
  if(request.method()==HttpMethod::Get){out.number(5,2);out.field("maximum_slots",NAWSERVER_API_KEYS_MAX);out.text("slots");out.number(4,NAWSERVER_API_KEYS_MAX);for(std::size_t i=0;i<NAWSERVER_API_KEYS_MAX;++i){const auto m=store_.metadata(i);out.number(5,4);out.field("slot",i);out.field("name",m.label.data());out.text("active");out.byte(m.active?0xf5:0xf4);out.field("revision",m.revision);}}
  else{const auto type=request.header("Content-Type");if(!type||type->substr(0,type->find(';'))!="application/cbor")return responseResult(response.text(415,"CBOR required"));std::string_view action,name;std::size_t slot=NAWSERVER_API_KEYS_MAX;if(!readCommand(request.body(),action,name,slot))return responseResult(response.text(400,"Invalid command"));keys::Secret secret{};keys::Status status=keys::Status::Invalid;if(action=="create")status=store_.create(name,secret);else if(action=="rotate")status=store_.rotate(slot,secret);else if(action=="revoke")status=store_.revoke(slot);if(status!=keys::Status::Ok)return responseResult(response.text(status==keys::Status::Full?409:status==keys::Status::Invalid||status==keys::Status::NotFound?400:503,"Key operation failed"));out.number(5,action=="revoke"?1:2);out.field("slot",action=="create"?secret.slot:slot);if(action!="revoke")out.field("secret",secret.value.data());std::memset(secret.value.data(),0,secret.value.size());}
  const auto bytes=out.view();return responseResult(response.send(200,"application/cbor",bytes));
 }
 void console(std::string_view command)
 {
  using namespace NaWServer;detail::LockGuard<detail::Mutex> lock(mutex_);
  if(command=="keys list"){for(std::size_t i=0;i<NAWSERVER_API_KEYS_MAX;++i){auto m=store_.metadata(i);printf("KEY slot=%u active=%u revision=%lu name=%s\n",unsigned(i),m.active,static_cast<unsigned long>(m.revision),m.label.data());}return;}
  keys::Secret secret{};auto status=keys::Status::Invalid;std::size_t slot=NAWSERVER_API_KEYS_MAX;
  if(command.substr(0,12)=="keys create ")status=store_.create(command.substr(12),secret);
  else{const bool rotate=command.substr(0,12)=="keys rotate ",revoke=command.substr(0,12)=="keys revoke ";if(rotate||revoke){const auto value=command.substr(12);const auto parsed=std::from_chars(value.data(),value.data()+value.size(),slot);if(parsed.ec==std::errc{}&&parsed.ptr==value.data()+value.size())status=rotate?store_.rotate(slot,secret):store_.revoke(slot);}}
  if(status==keys::Status::Ok){if(secret.value[0])printf("KEY slot=%u secret=%s\n",unsigned(secret.slot),secret.value.data());else printf("KEY slot=%u revoked\n",unsigned(slot));}else printf("KEY status=%u; :keys list | :keys create NAME | :keys rotate SLOT | :keys revoke SLOT\n",unsigned(status));std::memset(secret.value.data(),0,secret.value.size());
 }
};
inline NaWServer::AdmissionOptions admissionOptions()
{
 static const auto profiles=[] {std::array<NaWServer::AdmissionOptions::ReservedProfile,NAWSERVER_API_KEYS_MAX+1> p{};for(std::size_t i=0;i<NAWSERVER_API_KEYS_MAX;++i)p[i]={NaWServer::keys::Store::identity(i),{1,0,0,0,1000}};p[NAWSERVER_API_KEYS_MAX]={"naw-bootstrap",{NAWSERVER_DEMO_BOOTSTRAP_HTTP,0,0,0,1000}};return p;}();
 NaWServer::AdmissionOptions o;o.reserved_profiles=profiles.data();o.reserved_profile_count=profiles.size();o.shared={NAWSERVER_DEMO_SHARED_HTTP,1,1,0,1000};o.shared_clients=NAWSERVER_DEMO_SHARED_CLIENTS;o.idle_timeout_ms=1500;return o;
}
class Console
{
 Service &service_;std::array<char,96> line_{};std::size_t used_=0;bool active_=false,overflow_=false;
public:
 explicit Console(Service &service):service_(service){}
 bool input(int ch){if(ch<0)return false;if(!active_){if(ch!=':')return false;active_=true;used_=0;overflow_=false;return true;}if(ch=='\r'||ch=='\n'){if(!overflow_)service_.console({line_.data(),used_});else printf("KEY command too long\n");active_=false;}else if(used_<line_.size())line_[used_++]=static_cast<char>(ch);else overflow_=true;return true;}
};
}

#endif
