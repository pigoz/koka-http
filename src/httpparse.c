/*---------------------------------------------------------------------------
  HTTP/1.1 request parsing with llhttp (the parser of Node.js), for `httpparse.kk`.

  llhttp is used in its default strict mode: it rejects conflicting framing
  (`Content-Length` with `Transfer-Encoding`, duplicate `Content-Length`,
  non-numeric lengths, `chunked` that is not the final coding), bare LF line
  endings, obs-folding, and invalid header tokens -- the usual request
  smuggling vectors. Chunked bodies are decoded by llhttp.

  The callbacks collect the URL, the headers, and the body into a `kk_hp_t`;
  the parser pauses when the request head is complete and again when the
  request is complete, so the Koka side handles one request at a time
  (with pipelined input the rest stays unparsed until the next call).
---------------------------------------------------------------------------*/

#include <stdio.h>
#include <string.h>
#include <llhttp.h>

#if LLHTTP_VERSION_MAJOR != 9
#error "httpparse.c: expected the llhttp 9.x headers from vendor/llhttp/include (is KOKA_OPTIONS set? run through `mise exec`)"
#endif

#define KK_HP_NEED_MORE        0
#define KK_HP_HEAD_COMPLETE    1
#define KK_HP_MESSAGE_COMPLETE 2
#define KK_HP_ERROR          (-1)

#define KK_HP_MAX_HEADERS    100

typedef struct kk_hp_buf_s {
  char*  p;
  size_t len;
  size_t cap;
} kk_hp_buf_t;

typedef struct kk_hp_header_s {
  kk_hp_buf_t name;
  kk_hp_buf_t value;
} kk_hp_header_t;

typedef struct kk_hp_s {
  llhttp_t        parser;       // `parser.data` points back to us
  kk_hp_buf_t     url;
  kk_hp_header_t* headers;
  size_t          nheaders;
  size_t          cap_headers;
  bool            field_done;   // the current header name is complete (the next name starts a new header)
  kk_hp_buf_t     body;
  size_t          head_size;    // approximate size of the request head so far
  size_t          max_head;
  size_t          max_body;
  bool            in_message;   // between the first byte of a request and its end
  int             event;        // what paused the parser
  kk_ssize_t      consumed;     // bytes consumed by the last `kk_hp_execute`
  int             status;       // HTTP status for the last error
  const char*     reason;       // reason for a user error (static string)
} kk_hp_t;


//---------------------------------------------------------------------------
// Buffers
//---------------------------------------------------------------------------

static bool kk_hp_buf_append(kk_hp_buf_t* b, const char* at, size_t len) {
  if (len == 0) return true;
  if (b->len + len > b->cap) {
    size_t cap = (b->cap == 0 ? 64 : b->cap);
    while (cap < b->len + len) cap *= 2;
    char* p = (char*)kk_realloc(b->p, (kk_ssize_t)cap, kk_get_context());
    if (p == NULL) return false;
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->len, at, len);
  b->len += len;
  return true;
}

static void kk_hp_buf_free(kk_hp_buf_t* b) {
  if (b->p != NULL) kk_free(b->p, kk_get_context());
  b->p = NULL;
  b->len = b->cap = 0;
}

static kk_string_t kk_hp_buf_string(const kk_hp_buf_t* b, kk_context_t* ctx) {
  return kk_string_alloc_from_qutf8n((kk_ssize_t)b->len, (b->p == NULL ? "" : b->p), ctx);
}


//---------------------------------------------------------------------------
// Callbacks
//---------------------------------------------------------------------------

static kk_hp_t* kk_hp_of(llhttp_t* parser) {
  return (kk_hp_t*)parser->data;
}

static int kk_hp_fail(kk_hp_t* hp, int status, const char* reason) {
  hp->status = status;
  hp->reason = reason;
  llhttp_set_error_reason(&hp->parser, reason);
  return HPE_USER;
}

// Count head bytes (plus `extra` for separators) against the limit.
static int kk_hp_count_head(kk_hp_t* hp, size_t len, size_t extra) {
  hp->head_size += len + extra;
  if (hp->head_size > hp->max_head) return kk_hp_fail(hp, 431, "request head too large");
  return HPE_OK;
}

static int kk_hp_on_message_begin(llhttp_t* parser) {
  kk_hp_t* hp = kk_hp_of(parser);
  hp->url.len = 0;
  hp->nheaders = 0;
  hp->field_done = false;
  hp->body.len = 0;
  hp->head_size = 0;
  hp->in_message = true;
  return HPE_OK;
}

static int kk_hp_on_url(llhttp_t* parser, const char* at, size_t len) {
  kk_hp_t* hp = kk_hp_of(parser);
  int err = kk_hp_count_head(hp, len, 0);
  if (err != HPE_OK) return err;
  if (!kk_hp_buf_append(&hp->url, at, len)) return kk_hp_fail(hp, 500, "out of memory");
  return HPE_OK;
}

static int kk_hp_on_header_field(llhttp_t* parser, const char* at, size_t len) {
  kk_hp_t* hp = kk_hp_of(parser);
  int err = kk_hp_count_head(hp, len, 0);
  if (err != HPE_OK) return err;
  if (hp->nheaders == 0 || hp->field_done) {
    // a new header
    if (hp->nheaders >= KK_HP_MAX_HEADERS) return kk_hp_fail(hp, 431, "too many headers");
    if (hp->nheaders == hp->cap_headers) {
      size_t cap = (hp->cap_headers == 0 ? 16 : 2 * hp->cap_headers);
      kk_hp_header_t* hs = (kk_hp_header_t*)kk_realloc(hp->headers, (kk_ssize_t)(cap * sizeof(kk_hp_header_t)), kk_get_context());
      if (hs == NULL) return kk_hp_fail(hp, 500, "out of memory");
      memset(hs + hp->cap_headers, 0, (cap - hp->cap_headers) * sizeof(kk_hp_header_t));
      hp->headers = hs;
      hp->cap_headers = cap;
    }
    kk_hp_header_t* h = &hp->headers[hp->nheaders++];
    h->name.len = 0;
    h->value.len = 0;
    hp->field_done = false;
  }
  if (!kk_hp_buf_append(&hp->headers[hp->nheaders - 1].name, at, len)) return kk_hp_fail(hp, 500, "out of memory");
  return HPE_OK;
}

static int kk_hp_on_header_field_complete(llhttp_t* parser) {
  kk_hp_t* hp = kk_hp_of(parser);
  hp->field_done = true;
  return kk_hp_count_head(hp, 0, 4);  // ": " and CRLF
}

static int kk_hp_on_header_value(llhttp_t* parser, const char* at, size_t len) {
  kk_hp_t* hp = kk_hp_of(parser);
  int err = kk_hp_count_head(hp, len, 0);
  if (err != HPE_OK) return err;
  if (hp->nheaders == 0) return kk_hp_fail(hp, 400, "header value without a name");
  if (!kk_hp_buf_append(&hp->headers[hp->nheaders - 1].value, at, len)) return kk_hp_fail(hp, 500, "out of memory");
  return HPE_OK;
}

static int kk_hp_on_version_complete(llhttp_t* parser) {
  kk_hp_t* hp = kk_hp_of(parser);
  if (parser->http_major != 1 || parser->http_minor > 1) return kk_hp_fail(hp, 505, "HTTP version not supported");
  return HPE_OK;
}

static int kk_hp_on_headers_complete(llhttp_t* parser) {
  kk_hp_t* hp = kk_hp_of(parser);
  hp->event = KK_HP_HEAD_COMPLETE;
  return HPE_PAUSED;
}

static int kk_hp_on_body(llhttp_t* parser, const char* at, size_t len) {
  kk_hp_t* hp = kk_hp_of(parser);
  if (hp->body.len + len > hp->max_body) return kk_hp_fail(hp, 413, "request body too large");
  if (!kk_hp_buf_append(&hp->body, at, len)) return kk_hp_fail(hp, 500, "out of memory");
  return HPE_OK;
}

static int kk_hp_on_message_complete(llhttp_t* parser) {
  kk_hp_t* hp = kk_hp_of(parser);
  hp->in_message = false;
  hp->event = KK_HP_MESSAGE_COMPLETE;
  return HPE_PAUSED;
}

static llhttp_settings_t kk_hp_settings;
static bool kk_hp_settings_initialized = false;

static const llhttp_settings_t* kk_hp_get_settings(void) {
  if (!kk_hp_settings_initialized) {
    llhttp_settings_init(&kk_hp_settings);
    kk_hp_settings.on_message_begin       = &kk_hp_on_message_begin;
    kk_hp_settings.on_url                 = &kk_hp_on_url;
    kk_hp_settings.on_version_complete    = &kk_hp_on_version_complete;
    kk_hp_settings.on_header_field        = &kk_hp_on_header_field;
    kk_hp_settings.on_header_field_complete = &kk_hp_on_header_field_complete;
    kk_hp_settings.on_header_value        = &kk_hp_on_header_value;
    kk_hp_settings.on_headers_complete    = &kk_hp_on_headers_complete;
    kk_hp_settings.on_body                = &kk_hp_on_body;
    kk_hp_settings.on_message_complete    = &kk_hp_on_message_complete;
    kk_hp_settings_initialized = true;
  }
  return &kk_hp_settings;
}


//---------------------------------------------------------------------------
// Creating and running a parser
//---------------------------------------------------------------------------

static void kk_hp_free(void* p, kk_block_t* block, kk_context_t* ctx) {
  kk_unused(block);
  kk_hp_t* hp = (kk_hp_t*)p;
  if (hp == NULL) return;
  kk_hp_buf_free(&hp->url);
  kk_hp_buf_free(&hp->body);
  for (size_t i = 0; i < hp->cap_headers; i++) {
    kk_hp_buf_free(&hp->headers[i].name);
    kk_hp_buf_free(&hp->headers[i].value);
  }
  if (hp->headers != NULL) kk_free(hp->headers, ctx);
  kk_free(hp, ctx);
}

static kk_hp_t* kk_hp_unbox(kk_box_t b, kk_context_t* ctx) {
  return (kk_hp_t*)kk_cptr_raw_unbox_borrowed(b, ctx);
}

// A new request parser with limits on the head and body size.
kk_box_t kk_hp_new(kk_ssize_t max_head, kk_ssize_t max_body, kk_context_t* ctx) {
  kk_hp_t* hp = (kk_hp_t*)kk_zalloc(sizeof(kk_hp_t), ctx);
  if (hp == NULL) {
    kk_fatal_error(ENOMEM, "httpparse: out of memory");
  }
  hp->max_head = (max_head < 0 ? 0 : (size_t)max_head);
  hp->max_body = (max_body < 0 ? 0 : (size_t)max_body);
  llhttp_init(&hp->parser, HTTP_REQUEST, kk_hp_get_settings());
  hp->parser.data = hp;
  return kk_cptr_raw_box(&kk_hp_free, hp, ctx);
}

static int kk_hp_status_of(kk_hp_t* hp, llhttp_errno_t err) {
  switch (err) {
    case HPE_USER:            return (hp->status != 0 ? hp->status : 400);
    case HPE_INVALID_METHOD:  return 501;
    case HPE_PAUSED_H2_UPGRADE: return 505;   // the HTTP/2 connection preface
    default:                  return 400;
  }
}

// Parse the bytes `data[from..]` (borrowed). Returns one of the `KK_HP_` codes;
// `kk_hp_consumed` gives the number of bytes that were used.
int32_t kk_hp_execute(kk_box_t hpb, kk_bytes_t data, kk_ssize_t from, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  kk_ssize_t len = 0;
  const char* base = (const char*)kk_bytes_buf_borrow(data, &len, ctx);
  if (from < 0) from = 0;
  if (from > len) from = len;
  const char* start = base + from;
  const size_t n = (size_t)(len - from);
  if (llhttp_get_errno(&hp->parser) == HPE_PAUSED) llhttp_resume(&hp->parser);
  hp->event = KK_HP_NEED_MORE;
  const llhttp_errno_t err = llhttp_execute(&hp->parser, start, n);
  if (err == HPE_OK) {
    hp->consumed = (kk_ssize_t)n;
    return KK_HP_NEED_MORE;
  }
  const char* pos = llhttp_get_error_pos(&hp->parser);
  hp->consumed = (pos != NULL && pos >= start && pos <= start + n ? (kk_ssize_t)(pos - start) : (kk_ssize_t)n);
  if (err == HPE_PAUSED && hp->event != KK_HP_NEED_MORE) {
    return hp->event;
  }
  if (err == HPE_PAUSED_UPGRADE) {
    // a CONNECT or upgrade request was completed; we do not support upgrades (the connection is closed after the response)
    return KK_HP_MESSAGE_COMPLETE;
  }
  if (hp->status == 0) hp->status = kk_hp_status_of(hp, err);
  return KK_HP_ERROR;
}

kk_ssize_t kk_hp_consumed(kk_box_t hpb, kk_context_t* ctx) {
  return kk_hp_unbox(hpb, ctx)->consumed;
}

// Are we in the middle of a request (so an end-of-stream now is an error)?
bool kk_hp_in_message(kk_box_t hpb, kk_context_t* ctx) {
  return kk_hp_unbox(hpb, ctx)->in_message;
}

int32_t kk_hp_error_status(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  return (hp->status != 0 ? hp->status : 400);
}

kk_string_t kk_hp_error_reason(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  if (llhttp_get_errno(&hp->parser) == HPE_PAUSED_H2_UPGRADE) return kk_string_alloc_from_qutf8("HTTP/2 is not supported", ctx);
  const char* reason = llhttp_get_error_reason(&hp->parser);
  if (reason == NULL) reason = hp->reason;
  if (reason == NULL) reason = llhttp_errno_name(llhttp_get_errno(&hp->parser));
  return kk_string_alloc_from_qutf8(reason, ctx);
}


//---------------------------------------------------------------------------
// The current request
//---------------------------------------------------------------------------

kk_string_t kk_hp_method(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  return kk_string_alloc_from_qutf8(llhttp_method_name((llhttp_method_t)llhttp_get_method(&hp->parser)), ctx);
}

kk_string_t kk_hp_url(kk_box_t hpb, kk_context_t* ctx) {
  return kk_hp_buf_string(&kk_hp_unbox(hpb, ctx)->url, ctx);
}

kk_string_t kk_hp_version(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  char buf[32];
  snprintf(buf, sizeof(buf), "HTTP/%d.%d", (int)llhttp_get_http_major(&hp->parser), (int)llhttp_get_http_minor(&hp->parser));
  return kk_string_alloc_from_qutf8(buf, ctx);
}

kk_ssize_t kk_hp_header_count(kk_box_t hpb, kk_context_t* ctx) {
  return (kk_ssize_t)kk_hp_unbox(hpb, ctx)->nheaders;
}

kk_string_t kk_hp_header_name(kk_box_t hpb, kk_ssize_t i, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  if (i < 0 || (size_t)i >= hp->nheaders) return kk_string_empty();
  return kk_hp_buf_string(&hp->headers[i].name, ctx);
}

kk_string_t kk_hp_header_value(kk_box_t hpb, kk_ssize_t i, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  if (i < 0 || (size_t)i >= hp->nheaders) return kk_string_empty();
  return kk_hp_buf_string(&hp->headers[i].value, ctx);
}

// The declared body length: -1 for a chunked body.
int64_t kk_hp_content_length(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  if (hp->parser.flags & F_CHUNKED) return -1;
  if (hp->parser.flags & F_CONTENT_LENGTH) {
    return (hp->parser.content_length > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)hp->parser.content_length);
  }
  return 0;
}

// Can the connection be used for another request after this one?
bool kk_hp_keep_alive(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  return (llhttp_should_keep_alive(&hp->parser) != 0 && llhttp_get_upgrade(&hp->parser) == 0);
}

// Take the body of the completed request.
kk_bytes_t kk_hp_take_body(kk_box_t hpb, kk_context_t* ctx) {
  kk_hp_t* hp = kk_hp_unbox(hpb, ctx);
  if (hp->body.len == 0) return kk_bytes_empty();
  kk_bytes_t b = kk_bytes_alloc_dupn((kk_ssize_t)hp->body.len, (const uint8_t*)hp->body.p, ctx);
  if (hp->body.cap > 64 * 1024) kk_hp_buf_free(&hp->body);  // don't keep large buffers around between requests
                          else hp->body.len = 0;
  return b;
}
