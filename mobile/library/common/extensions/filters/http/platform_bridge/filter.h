#pragma once

#include "envoy/http/filter.h"

#include "common/common/logger.h"

#include "extensions/filters/http/common/pass_through_filter.h"

#include "library/common/extensions/filters/http/platform_bridge/c_types.h"
#include "library/common/extensions/filters/http/platform_bridge/filter.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace PlatformBridge {

class PlatformBridgeFilterConfig {
public:
  PlatformBridgeFilterConfig(
      const envoymobile::extensions::filters::http::platform_bridge::PlatformBridge& proto_config);

  const std::string& filter_name() { return filter_name_; }
  const envoy_http_filter* platform_filter() const { return platform_filter_; }

private:
  const std::string filter_name_;
  const envoy_http_filter* platform_filter_;
};

typedef std::shared_ptr<PlatformBridgeFilterConfig> PlatformBridgeFilterConfigSharedPtr;

enum class IterationState { Ongoing, Stopped };

/**
 * Harness to bridge Envoy filter invocations up to the platform layer.
 *
 * This filter enables filter implementations to be written in high-level platform-specific
 * languages and run within the Envoy filter chain. To mirror platform API conventions, the
 * semantic structure of platform filters differs slightly from Envoy filters. Platform
 * filter invocations (on-headers, on-data, etc.) receive *immutable* entities as parameters
 * and are expected to return compound results that include both the filter status, as well
 * as any desired modifications to the HTTP entity. Additionally, when platform filters
 * stop iteration, they _must_ use a new ResumeIteration status to resume iteration
 * at a later point. The Continue status is only valid if iteration is already ongoing.
 *
 * For more information on implementing platform filters, see the docs.
 */
class PlatformBridgeFilter final : public Http::PassThroughFilter,
                                   Logger::Loggable<Logger::Id::filter> {
public:
  PlatformBridgeFilter(PlatformBridgeFilterConfigSharedPtr config);

  // StreamFilterBase
  void onDestroy() override;

  // StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;

  // StreamEncoderFilter
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap& trailers) override;

private:
  static void replaceHeaders(Http::HeaderMap& headers, envoy_headers c_headers);
  Http::FilterHeadersStatus onHeaders(Http::HeaderMap& headers, bool end_stream,
                                      envoy_filter_on_headers_f on_headers);
  Http::FilterDataStatus onData(Buffer::Instance& data, bool end_stream,
                                Buffer::Instance* internal_buffer,
                                Http::HeaderMap** pending_headers, envoy_filter_on_data_f on_data);
  Http::FilterTrailersStatus onTrailers(Http::HeaderMap& trailers,
                                        Buffer::Instance* internal_buffer,
                                        Http::HeaderMap** pending_headers,
                                        envoy_filter_on_trailers_f on_trailers);
  const std::string filter_name_;
  IterationState iteration_state_;
  envoy_http_filter platform_filter_;
  Http::HeaderMap* pending_request_headers_{};
  Http::HeaderMap* pending_response_headers_{};
  Http::HeaderMap* pending_request_trailers_{};
  Http::HeaderMap* pending_response_trailers_{};
};

} // namespace PlatformBridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy