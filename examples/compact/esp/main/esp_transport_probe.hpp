// Diagnostic overlay only: composition preserves the native ownership contract.
#pragma once
#include <NaWServer/backends/esp_idf_tcp.h>
#include <esp_timer.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace compact
{
 class TransportProbe final : public NaWServer::tcp::Transport
 {
  using Handle=NaWServer::tcp::Handle;
  NaWServer::tcp::IdfTransport native_;
  std::atomic<std::uint32_t> accepted_{0},failures_{0},enfile_{0},emfile_{0},setup_failures_{0},owned_max_{0};
  std::atomic<int> last_error_{0},last_status_{0};
  std::atomic<std::uint32_t> last_error_ms_{0};
  std::atomic<bool> counter_overflow_{false};

  void increment(std::atomic<std::uint32_t> &counter)
  {
   // Accept/listener lifecycle has one writer; UART sampling only loads.
   const auto value=counter.load(std::memory_order_relaxed);
   if(value==std::numeric_limits<std::uint32_t>::max())counter_overflow_.store(true,std::memory_order_relaxed);
   else counter.store(value+1,std::memory_order_relaxed);
  }
  void observeOwned()
  {
   const auto current=static_cast<std::uint32_t>(native_.ownedHandles());
   if(current>owned_max_.load(std::memory_order_relaxed))owned_max_.store(current,std::memory_order_relaxed);
  }
 public:
  NaWServer::tcp::OperationResult start(){return native_.start();}
  NaWServer::tcp::OperationResult stop(){return native_.stop();}
  std::size_t ownedHandles() const{return native_.ownedHandles();}
  NaWServer::tcp::ListenerResult openListener(const char *address,std::uint16_t port,int backlog) override
  {auto value=native_.openListener(address,port,backlog);observeOwned();return value;}
  NaWServer::tcp::AcceptResult accept(Handle listener) override
  {
   const auto value=native_.accept(listener);
   observeOwned();
   if(value.status==NaWServer::tcp::Status::Ok)increment(accepted_);
   else if(value.status!=NaWServer::tcp::Status::WouldBlock&&value.status!=NaWServer::tcp::Status::Interrupted)
   {
    increment(failures_);
    if(value.native_error==ENFILE)increment(enfile_);
    if(value.native_error==EMFILE)increment(emfile_);
    if(value.cleanup.has_value())increment(setup_failures_);
    last_error_.store(value.native_error,std::memory_order_relaxed);
    last_status_.store(static_cast<int>(value.status),std::memory_order_relaxed);
    const auto milliseconds=static_cast<std::uint64_t>(esp_timer_get_time()/1000);
    if(milliseconds>std::numeric_limits<std::uint32_t>::max())counter_overflow_.store(true,std::memory_order_relaxed);
    last_error_ms_.store(milliseconds>std::numeric_limits<std::uint32_t>::max()?
      std::numeric_limits<std::uint32_t>::max():static_cast<std::uint32_t>(milliseconds),std::memory_order_relaxed);
   }
   return value;
  }
  int listenerError(Handle handle) override{return native_.listenerError(handle);}
  NaWServer::tcp::PeerAddress peerAddress(Handle handle) override{return native_.peerAddress(handle);}
  NaWServer::tcp::IoResult readSome(Handle handle,std::uint8_t *data,std::size_t bytes) override
  {return native_.readSome(handle,data,bytes);}
  NaWServer::tcp::IoResult writeSome(Handle handle,const std::uint8_t *data,std::size_t bytes) override
  {return native_.writeSome(handle,data,bytes);}
  NaWServer::tcp::WaitResult wait(const NaWServer::tcp::Interest *interests,NaWServer::tcp::Ready *ready,
    std::size_t count,std::uint32_t timeout) override{return native_.wait(interests,ready,count,timeout);}
  NaWServer::tcp::OperationResult shutdownWrite(Handle handle) override{return native_.shutdownWrite(handle);}
  NaWServer::tcp::CloseResult close(Handle &handle) override{return native_.close(handle);}
  void sample(std::uint32_t cycle) const
  {
   printf("NATIVE_ACCEPT_PROBE cycle=%u accepted=%u failures=%u enfile=%u emfile=%u setup_failures=%u last_error=%d last_status=%d last_error_ms=%u owned=%u observed_owned_max=%u counter_overflow=%u\n",
    unsigned(cycle),unsigned(accepted_.load()),unsigned(failures_.load()),unsigned(enfile_.load()),unsigned(emfile_.load()),
    unsigned(setup_failures_.load()),last_error_.load(),last_status_.load(),unsigned(last_error_ms_.load()),
    unsigned(native_.ownedHandles()),unsigned(owned_max_.load()),unsigned(counter_overflow_.load()));
  }
 };
}
