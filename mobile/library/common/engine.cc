#include "library/common/engine.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/common/lock_guard.h"

#include "library/common/bridge/utility.h"
#include "library/common/config/internal.h"
#include "library/common/data/utility.h"
#include "library/common/network/android.h"
#include "library/common/stats/utility.h"

namespace Envoy {

Engine::Engine(envoy_engine_callbacks callbacks, envoy_logger logger,
               envoy_event_tracker event_tracker)
    : callbacks_(callbacks), logger_(logger), event_tracker_(event_tracker),
      dispatcher_(std::make_unique<Event::ProvisionalDispatcher>()) {
  ExtensionRegistry::registerFactories();

  // TODO(Augustyniak): Capturing an address of event_tracker_ and registering it in the API
  // registry may lead to crashes at Engine shutdown. To be figured out as part of
  // https://github.com/envoyproxy/envoy-mobile/issues/332
  Envoy::Api::External::registerApi(std::string(envoy_event_tracker_api_name), &event_tracker_);
}

envoy_status_t Engine::run(const std::string config, const std::string log_level,
                           const std::string admin_address_path) {
  // Start the Envoy on the dedicated thread. Note: due to how the assignment operator works with
  // std::thread, main_thread_ is the same object after this call, but its state is replaced with
  // that of the temporary. The temporary object's state becomes the default state, which does
  // nothing.
  auto options = std::make_unique<Envoy::OptionsImpl>();
  options->setConfigYaml(absl::StrCat(config_header, config));
  if (!log_level.empty()) {
    options->setLogLevel(options->parseAndValidateLogLevel(log_level.c_str()));
  }
  options->setConcurrency(1);
  if (!admin_address_path.empty()) {
    options->setAdminAddressPath(admin_address_path);
  }
  return run(std::move(options));
}

envoy_status_t Engine::run(std::unique_ptr<Envoy::OptionsImpl>&& options) {
  main_thread_ = std::thread(&Engine::main, this, std::move(options));
  return ENVOY_SUCCESS;
}

envoy_status_t Engine::main(std::unique_ptr<Envoy::OptionsImpl>&& options) {
  // Using unique_ptr ensures main_common's lifespan is strictly scoped to this function.
  std::unique_ptr<EngineCommon> main_common;
  {
    Thread::LockGuard lock(mutex_);
    try {
      if (event_tracker_.track != nullptr) {
        assert_handler_registration_ =
            Assert::addDebugAssertionFailureRecordAction([this](const char* location) {
              const auto event = Bridge::Utility::makeEnvoyMap(
                  {{"name", "assertion"}, {"location", std::string(location)}});
              event_tracker_.track(event, event_tracker_.context);
            });
        bug_handler_registration_ =
            Assert::addEnvoyBugFailureRecordAction([this](const char* location) {
              const auto event = Bridge::Utility::makeEnvoyMap(
                  {{"name", "bug"}, {"location", std::string(location)}});
              event_tracker_.track(event, event_tracker_.context);
            });
      }

      // We let the thread clean up this log delegate pointer
      if (logger_.log) {
        log_delegate_ptr_ =
            std::make_unique<Logger::LambdaDelegate>(logger_, Logger::Registry::getSink());
      } else {
        log_delegate_ptr_ =
            std::make_unique<Logger::DefaultDelegate>(log_mutex_, Logger::Registry::getSink());
      }

      main_common = std::make_unique<EngineCommon>(std::move(options));
      server_ = main_common->server();
      event_dispatcher_ = &server_->dispatcher();

      cv_.notifyAll();
    } catch (const Envoy::NoServingException& e) {
      PANIC(e.what());
    } catch (const Envoy::MalformedArgvException& e) {
      PANIC(e.what());
    } catch (const Envoy::EnvoyException& e) {
      PANIC(e.what());
    }

    // Note: We're waiting longer than we might otherwise to drain to the main thread's dispatcher.
    // This is because we're not simply waiting for its availability and for it to have started, but
    // also because we're waiting for clusters to have done their first attempt at DNS resolution.
    // When we improve synchronous failure handling and/or move to dynamic forwarding, we only need
    // to wait until the dispatcher is running (and can drain by enqueueing a drain callback on it,
    // as we did previously).

    postinit_callback_handler_ = main_common->server()->lifecycleNotifier().registerCallback(
        Envoy::Server::ServerLifecycleNotifier::Stage::PostInit, [this]() -> void {
          ASSERT(Thread::MainThread::isMainOrTestThread());

          connectivity_manager_ =
              Network::ConnectivityManagerFactory{server_->serverFactoryContext()}.get();
          if (Api::OsSysCallsSingleton::get().supportsGetifaddrs()) {
            Envoy::Network::Android::Utility::setAlternateGetifaddrs();
          }
          auto v4_interfaces = connectivity_manager_->enumerateV4Interfaces();
          auto v6_interfaces = connectivity_manager_->enumerateV6Interfaces();
          logInterfaces("netconf_get_v4_interfaces", v4_interfaces);
          logInterfaces("netconf_get_v6_interfaces", v6_interfaces);
          client_scope_ = server_->serverFactoryContext().scope().createScope("pulse.");
          // StatNameSet is lock-free, the benefit of using it is being able to create StatsName
          // on-the-fly without risking contention on system with lots of threads.
          // It also comes with ease of programming.
          stat_name_set_ = client_scope_->symbolTable().makeSet("pulse");
          auto api_listener = server_->listenerManager().apiListener()->get().http();
          ASSERT(api_listener.has_value());
          http_client_ = std::make_unique<Http::Client>(api_listener.value(), *dispatcher_,
                                                        server_->serverFactoryContext().scope(),
                                                        server_->api().randomGenerator());
          dispatcher_->drain(server_->dispatcher());
          if (callbacks_.on_engine_running != nullptr) {
            callbacks_.on_engine_running(callbacks_.context);
          }
        });
  } // mutex_

  // The main run loop must run without holding the mutex, so that the destructor can acquire it.
  bool run_success = main_common->run();
  // The above call is blocking; at this point the event loop has exited.

  // Ensure destructors run on Envoy's main thread.
  postinit_callback_handler_.reset(nullptr);
  connectivity_manager_.reset();
  client_scope_.reset();
  stat_name_set_.reset();
  main_common.reset(nullptr);
  bug_handler_registration_.reset(nullptr);
  assert_handler_registration_.reset(nullptr);

  callbacks_.on_exit(callbacks_.context);

  return run_success ? ENVOY_SUCCESS : ENVOY_FAILURE;
}

envoy_status_t Engine::terminate() {
  // If main_thread_ has finished (or hasn't started), there's nothing more to do.
  if (!main_thread_.joinable()) {
    return ENVOY_FAILURE;
  }

  // We need to be sure that MainCommon is finished being constructed so we can dispatch shutdown.
  {
    Thread::LockGuard lock(mutex_);

    if (!event_dispatcher_) {
      cv_.wait(mutex_);
    }

    ASSERT(event_dispatcher_);
    ASSERT(dispatcher_);

    // Exit the event loop and finish up in Engine::run(...)
    if (std::this_thread::get_id() == main_thread_.get_id()) {
      // TODO(goaway): figure out some way to support this.
      PANIC("Terminating the engine from its own main thread is currently unsupported.");
    } else {
      dispatcher_->terminate();
    }
  } // lock(_mutex)

  if (std::this_thread::get_id() != main_thread_.get_id()) {
    main_thread_.join();
  }

  return ENVOY_SUCCESS;
}

Engine::~Engine() { terminate(); }

envoy_status_t Engine::recordCounterInc(const std::string& elements, envoy_stats_tags tags,
                                        uint64_t count) {
  ENVOY_LOG(trace, "[pulse.{}] recordCounterInc", elements);
  ASSERT(dispatcher_->isThreadSafe(), "pulse calls must run from dispatcher's context");
  Stats::StatNameTagVector tags_vctr =
      Stats::Utility::transformToStatNameTagVector(tags, stat_name_set_);
  std::string name = Stats::Utility::sanitizeStatsName(elements);
  Stats::Utility::counterFromElements(*client_scope_, {Stats::DynamicName(name)}, tags_vctr)
      .add(count);
  return ENVOY_SUCCESS;
}

envoy_status_t Engine::makeAdminCall(absl::string_view path, absl::string_view method,
                                     envoy_data& out) {
  ENVOY_LOG(trace, "admin call {} {}", method, path);
  if (!server_->admin()) {
    ENVOY_LOG(warn, "admin support compiled out.");
    return ENVOY_FAILURE;
  }

  ASSERT(dispatcher_->isThreadSafe(), "admin calls must be run from the dispatcher's context");
  auto response_headers = Http::ResponseHeaderMapImpl::create();
  std::string body;
  const auto code = server_->admin()->request(path, method, *response_headers, body);
  if (code != Http::Code::OK) {
    ENVOY_LOG(warn, "admin call failed with status {} body {}", static_cast<uint64_t>(code), body);
    return ENVOY_FAILURE;
  }

  out = Data::Utility::copyToBridgeData(body);

  return ENVOY_SUCCESS;
}

Event::ProvisionalDispatcher& Engine::dispatcher() { return *dispatcher_; }

Http::Client& Engine::httpClient() {
  RELEASE_ASSERT(dispatcher_->isThreadSafe(),
                 "httpClient must be accessed from dispatcher's context");
  return *http_client_;
}

Network::ConnectivityManager& Engine::networkConnectivityManager() {
  RELEASE_ASSERT(dispatcher_->isThreadSafe(),
                 "networkConnectivityManager must be accessed from dispatcher's context");
  return *connectivity_manager_;
}

void Engine::flushStats() {
  ASSERT(dispatcher_->isThreadSafe(), "flushStats must be called from the dispatcher's context");

  server_->flushStats();
}

Upstream::ClusterManager& Engine::getClusterManager() {
  ASSERT(dispatcher_->isThreadSafe(),
         "getClusterManager must be called from the dispatcher's context");
  return server_->clusterManager();
}

Stats::Store& Engine::getStatsStore() {
  ASSERT(dispatcher_->isThreadSafe(), "getStatsStore must be called from the dispatcher's context");
  return server_->stats();
}

void Engine::logInterfaces(absl::string_view event,
                           std::vector<Network::InterfacePair>& interfaces) {
  std::vector<std::string> names;
  names.resize(interfaces.size());
  std::transform(interfaces.begin(), interfaces.end(), names.begin(),
                 [](Network::InterfacePair& pair) { return std::get<0>(pair); });

  auto unique_end = std::unique(names.begin(), names.end());
  std::string all_names = std::accumulate(names.begin(), unique_end, std::string{},
                                          [](std::string acc, std::string next) {
                                            return acc.empty() ? next : std::move(acc) + "," + next;
                                          });
  ENVOY_LOG_EVENT(debug, event, all_names);
}

} // namespace Envoy
