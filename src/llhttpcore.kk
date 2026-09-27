/*---------------------------------------------------------------------------
  The generated llhttp state machine (vendor/llhttp/src/llhttp.c, v9.4.3).

  It must be compiled on its own: it declares llhttp's internal callbacks with
  types that differ from their definitions in `api.c` and `http.c` (included by
  `httpparse.kk`), which is fine across compilation units but not within one.
---------------------------------------------------------------------------*/
module llhttpcore

extern import
  c file "../vendor/llhttp/src/llhttp.c"
