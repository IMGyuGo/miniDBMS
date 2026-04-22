#ifndef HTTP_MESSAGE_H
#define HTTP_MESSAGE_H

#include <stddef.h>

#include "../../include/api_contract.h"
#include "../../include/db_service.h"

/*
 * Role C HTTP message layer
 *
 * 이 헤더는 transport wire format 과 service result 사이를 이어 주는
 * 메시지 계층 공개 API를 정의한다.
 *
 * 규칙:
 *   - 소켓 accept loop, worker queue, 락은 다루지 않는다.
 *   - HTTP body 파싱과 응답 직렬화만 담당한다.
 */

int http_parse_query_request(const char *method,
                             const char *path,
                             const char *body,
                             ApiQueryRequest *out,
                             ApiResponseMeta *error_meta);

void http_response_meta_from_service(const DBServiceResult *service_result,
                                     ApiResponseMeta *meta);

int http_serialize_query_response(const ApiResponseMeta *meta,
                                  const DBServiceResult *service_result,
                                  char *buffer,
                                  size_t buffer_size);

int http_serialize_health_response(char *buffer, size_t buffer_size);

#endif /* HTTP_MESSAGE_H */
