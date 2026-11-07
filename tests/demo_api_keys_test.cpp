// SPDX-License-Identifier: MIT
#include "../examples/common/demo_api_keys.hpp"
#include <NaWServer/http_server.h>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/route.h>
#include <vector>
#include <string>
#include <cstdlib>
#define CHECK(x) do {if(!(x))std::abort();}while(false)
using namespace NaWServer;
namespace
{
class Disk:public keys::Storage
{
 std::array<std::vector<std::uint8_t>,2> data_;
public:
 keys::ReadStatus read(unsigned i,void *out,std::size_t cap,std::size_t &size)override{size=data_[i].size();if(!size)return keys::ReadStatus::Missing;if(size>cap)return keys::ReadStatus::Error;std::memcpy(out,data_[i].data(),size);return keys::ReadStatus::Ok;}
 bool write(unsigned i,const void *bytes,std::size_t size)override{const auto *p=static_cast<const std::uint8_t*>(bytes);data_[i].assign(p,p+size);return true;}
};
class Random:public keys::Random{unsigned next_=0;bool fill(std::uint8_t *out,std::size_t size)override{while(size--)*out++=static_cast<std::uint8_t>(++next_);return true;}};
class Exchange:public HttpExchange
{
 static SendResult send(void *ctx,int status,const char*,const Header*,std::size_t,ByteView bytes){auto &s=*static_cast<Exchange*>(ctx);s.status=status;s.output.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());return SendResult::Ok;}
public:
 HttpMethod method=HttpMethod::Get;std::string credential,key,output,origin;std::vector<std::uint8_t> body;int status=0;
 HttpRequestHead head()const override{return {method,"/api/v2/keys",body.size()};}
 HeaderReadResult readHeader(const char *name,char *out,std::size_t cap)const override{std::string_view value;if(std::strcmp(name,"Authorization")==0)value=credential;else if(std::strcmp(name,"X-Client-Key")==0)value=key;else if(std::strcmp(name,"Origin")==0)value=origin;else if(std::strcmp(name,"Host")==0)value="device";else if(std::strcmp(name,"Content-Type")==0)value="application/cbor";else return {HeaderReadStatus::NotFound,0};if(value.empty())return {HeaderReadStatus::NotFound,0};if(cap<=value.size())return {HeaderReadStatus::BufferTooSmall,value.size()};std::memcpy(out,value.data(),value.size());out[value.size()]=0;return {HeaderReadStatus::Ok,value.size()};}
 BodyReadResult readBody(std::uint8_t*,std::size_t)override{std::abort();}
 std::optional<ByteView> preparedBody()const override{return ByteView{body.data(),body.size()};}
 ResponseTransport responseTransport()override{return {this,send};}
 std::optional<std::uint32_t> peerAddress()const override{return 0xc0a80101;}
};
class Backend:public HttpBackend{HttpError start(HttpRequestHandler&,const HttpLimits&)override{return HttpError::Ok;}HttpError stop()override{return HttpError::Ok;}};
}
int main()
{
 Admission<4> admission(nawserver_demo_keys::admissionOptions());CHECK(admission.valid());auto boot=admission.acquire("naw-bootstrap",AdmissionResource::Http,0);CHECK(boot.decision==AdmissionDecision::Accepted);CHECK(admission.reserved(boot.ticket));CHECK(admission.release(boot.ticket,0));
 Disk disk;Random random;keys::Store store(disk,random);nawserver_demo_keys::Service service(store);CHECK(service.initialize());keys::Secret secret;CHECK(store.create("test",secret)==keys::Status::Ok);
 Exchange exchange;HeaderRequest header(exchange);Response response({});AccessPolicy policy{};HeaderContext context{header,response,policy};char identity[65]{};
 exchange.key=secret.value.data();CHECK(nawserver_demo_keys::Service::resolve(&service,context,identity,sizeof(identity)).status==HeaderReadStatus::Ok);CHECK(std::string_view(identity)==keys::Store::identity(0));
 exchange.key=keys::Store::identity(0);CHECK(nawserver_demo_keys::Service::resolve(&service,context,identity,sizeof(identity)).status==HeaderReadStatus::InvalidArgument);
 exchange.key.clear();CHECK(nawserver_demo_keys::Service::resolve(&service,context,identity,sizeof(identity)).status==HeaderReadStatus::Ok);CHECK(std::string_view(identity)=="naw-bootstrap");
 FixedBasicAuthenticator users("demo","showcase-only",1);AuthMiddleware auth(users);AccessMiddleware access;auto chain=makeHeaderChain(auth,access);auto endpoint=[&](Context &ctx){return service.http(ctx);};Route route("/api/v2/keys",HttpMethod::Any,EndpointRef(endpoint),{},AccessPolicy::authenticated(),{},chain);HandlerBase *handlers[]{&route};Backend backend;HttpServer server(backend,handlers);
 server.handle(exchange);CHECK(exchange.status==401);exchange.credential="Basic ZGVtbzpzaG93Y2FzZS1vbmx5";server.handle(exchange);CHECK(exchange.status==200);CHECK(exchange.output.find(secret.value.data())==std::string::npos);
 exchange.method=HttpMethod::Post;exchange.body={0xa2,0x66,'a','c','t','i','o','n',0x66,'r','e','v','o','k','e',0x64,'s','l','o','t',0x00};server.handle(exchange);CHECK(exchange.status==200);CHECK(!store.authenticate(secret.value.data()));
 exchange.body={0xa1,0x66,'a','c','t','i','o','n',0x66,'r','e','v','o','k','e'};server.handle(exchange);CHECK(exchange.status==400);
  exchange.origin="http://malicious";server.handle(exchange);CHECK(exchange.status==403);
 keys::Store unopened(disk,random);nawserver_demo_keys::Service unavailable(unopened);auto bad_endpoint=[&](Context &ctx){return unavailable.http(ctx);};Route bad_route("/api/v2/keys",HttpMethod::Any,EndpointRef(bad_endpoint));HandlerBase *bad_handlers[]{&bad_route};HttpServer bad_server(backend,bad_handlers);exchange.origin.clear();bad_server.handle(exchange);CHECK(exchange.status==503);
 puts("DEMO_API_KEYS_PASS");
}
