#include <assert.h>
#include <bare.h>
#include <js.h>
#include <limits.h>
#include <math.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

typedef utf8_t sqlite3_native_path_t[4096];

typedef void (*sqlite3_native_dlsym_t)(void);

typedef struct {
  sqlite3 *handle;

  js_env_t *env;

  js_threadsafe_function_t *on_result;
} sqlite3_native_t;

typedef struct {
  sqlite3_vfs handle;

  char name[64];
  char dlerror[256];

  js_env_t *env;
  js_ref_t *ctx;

  js_threadsafe_function_t *on_access;
  js_threadsafe_function_t *on_size;
  js_threadsafe_function_t *on_read;
  js_threadsafe_function_t *on_write;
  js_threadsafe_function_t *on_delete;

  uv_sem_t done;
} sqlite3_native_vfs_t;

typedef struct {
  sqlite3_file handle;

  int type;

  sqlite3_native_vfs_t *vfs;
} sqlite3_native_file_t;

typedef struct {
  sqlite3_native_file_t *file;

  void *buf;
  int len;
  int64_t offset;

  int status;
} sqlite3_native_read_t;

typedef struct {
  sqlite3_native_file_t *file;

  const void *buf;
  int len;
  int64_t offset;

  int status;
} sqlite3_native_write_t;

typedef struct {
  sqlite3_native_file_t *file;

  int64_t size;

  int status;
} sqlite3_native_size_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  int flags;
  bool exists;

  int status;
} sqlite3_native_access_t;

typedef struct {
  sqlite3_native_vfs_t *vfs;

  const char *name;
  bool sync;

  int status;
} sqlite3_native_delete_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  sqlite3_native_path_t name;
  sqlite3_native_vfs_t *vfs;
  bool extensions;
} sqlite3_native_open_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;
} sqlite3_native_close_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  utf8_t *query;

  js_ref_t *result;
  uint32_t i;

  int len;
  char **rows;
  char **columns;

  char *error;

  uv_sem_t done;
} sqlite3_native_exec_t;

typedef enum {
  SQLITE3_NATIVE_QUERY_RUN,
  SQLITE3_NATIVE_QUERY_ALL,
  SQLITE3_NATIVE_QUERY_VALUES,
  SQLITE3_NATIVE_QUERY_GET,
} sqlite3_native_query_mode_t;

typedef enum {
  SQLITE3_NATIVE_VALUE_NULL,
  SQLITE3_NATIVE_VALUE_INTEGER,
  SQLITE3_NATIVE_VALUE_REAL,
  SQLITE3_NATIVE_VALUE_TEXT,
  SQLITE3_NATIVE_VALUE_BLOB,
} sqlite3_native_value_type_t;

typedef struct {
  sqlite3_native_value_type_t type;
  int64_t integer;
  double real;
  void *bytes;
  int len;
} sqlite3_native_query_param_t;

typedef struct {
  sqlite3_native_value_type_t type;
  int64_t integer;
  double real;
  void *bytes;
  int len;
} sqlite3_native_query_value_t;

typedef struct {
  sqlite3_native_query_value_t *values;
} sqlite3_native_query_row_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  utf8_t *query;
  sqlite3_native_query_param_t *params;
  int params_len;
  sqlite3_native_query_mode_t mode;

  int column_count;
  char **column_names;
  int row_count;
  int row_capacity;
  sqlite3_native_query_row_t *rows;

  int64_t changes;
  int64_t last_insert_rowid;

  char *error;
} sqlite3_native_query_t;

typedef struct {
  uv_work_t handle;

  sqlite3_native_t *db;

  js_deferred_t *deferred;

  utf8_t *path;
  utf8_t *entry;

  char *error;
} sqlite3_native_load_extension_t;

static const size_t sqlite3_native__queue_limit = 64;
static const double sqlite3_native__max_safe_integer = 9007199254740991.0;
static const double sqlite3_native__min_safe_integer = -9007199254740991.0;

static bool
sqlite3_native__ends_with(const char *string, const char *suffix) {
  size_t string_len = strlen(string);
  size_t suffix_len = strlen(suffix);

  if (suffix_len > string_len) return false;

  return strncmp(string + string_len - suffix_len, suffix, suffix_len) == 0;
}

static bool
sqlite3_native__is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

static bool
sqlite3_native__is_super_journal(const char *name) {
  size_t len = strlen(name);

  if (len < 12) return false;

  const char *suffix = name + len - 12;

  if (suffix[0] != '-' || suffix[1] != 'm' || suffix[2] != 'j') return false;
  if (suffix[9] != '9') return false;

  for (size_t i = 3; i < 9; i++) {
    if (!sqlite3_native__is_hex_digit(suffix[i])) return false;
  }

  for (size_t i = 10; i < 12; i++) {
    if (!sqlite3_native__is_hex_digit(suffix[i])) return false;
  }

  return true;
}

enum {
  SQLITE3_NATIVE_FILE_MAIN_DB = 0,
  SQLITE3_NATIVE_FILE_MAIN_JOURNAL = 1,
  SQLITE3_NATIVE_FILE_WAL = 2,
  SQLITE3_NATIVE_FILE_TEMP_DB = 3,
  SQLITE3_NATIVE_FILE_TEMP_JOURNAL = 4,
  SQLITE3_NATIVE_FILE_TRANSIENT_DB = 5,
  SQLITE3_NATIVE_FILE_SUBJOURNAL = 6,
  SQLITE3_NATIVE_FILE_SUPER_JOURNAL = 7,
};

static int
sqlite3_native__get_file_type(int flags) {
  if (flags & SQLITE_OPEN_MAIN_DB) return SQLITE3_NATIVE_FILE_MAIN_DB;
  if (flags & SQLITE_OPEN_MAIN_JOURNAL) return SQLITE3_NATIVE_FILE_MAIN_JOURNAL;
  if (flags & SQLITE_OPEN_WAL) return SQLITE3_NATIVE_FILE_WAL;
  if (flags & SQLITE_OPEN_TEMP_DB) return SQLITE3_NATIVE_FILE_TEMP_DB;
  if (flags & SQLITE_OPEN_TEMP_JOURNAL) return SQLITE3_NATIVE_FILE_TEMP_JOURNAL;
  if (flags & SQLITE_OPEN_TRANSIENT_DB) return SQLITE3_NATIVE_FILE_TRANSIENT_DB;
  if (flags & SQLITE_OPEN_SUBJOURNAL) return SQLITE3_NATIVE_FILE_SUBJOURNAL;
  if (flags & SQLITE_OPEN_SUPER_JOURNAL) return SQLITE3_NATIVE_FILE_SUPER_JOURNAL;

  return -1;
}

static int
sqlite3_native__get_file_type_from_name(const char *name) {
  if (sqlite3_native__ends_with(name, "-journal")) return SQLITE3_NATIVE_FILE_MAIN_JOURNAL;
  if (sqlite3_native__ends_with(name, "-wal")) return SQLITE3_NATIVE_FILE_WAL;
  if (sqlite3_native__is_super_journal(name)) return SQLITE3_NATIVE_FILE_SUPER_JOURNAL;

  return SQLITE3_NATIVE_FILE_MAIN_DB;
}

static int
sqlite3_native__error_from(js_env_t *env, js_value_t *value, int code) {
  int err;

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  assert(err == 0);

  if (type == js_null || type == js_undefined) return SQLITE_OK;

  return code;
}

static int
sqlite3_native__on_vfs_close(sqlite3_file *handle) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_read_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_read_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_READ);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_read_call(js_env_t *env, js_value_t *on_read, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_read_t *data = (sqlite3_native_read_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[4];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, data->buf, data->len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, data->offset, &args[2]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_read_done, (void *) data, &args[3]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_read, 4, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_read(sqlite3_file *handle, void *buf, int len, sqlite3_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_read_t data = {
    file,
    buf,
    len,
    offset
  };

  err = js_call_threadsafe_function(vfs->on_read, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static js_value_t *
sqlite3_native__on_vfs_write_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_write_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_WRITE);

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_write_call(js_env_t *env, js_value_t *on_write, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_write_t *data = (sqlite3_native_write_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[4];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_external_arraybuffer(env, (void *) data->buf, data->len, NULL, NULL, &args[1]);
  assert(err == 0);

  err = js_create_int64(env, data->offset, &args[2]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_write_done, (void *) data, &args[3]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_write, 4, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_write(sqlite3_file *handle, const void *buf, int len, sqlite_int64 offset) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_write_t data = {
    file,
    buf,
    len,
    offset
  };

  err = js_call_threadsafe_function(vfs->on_write, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static int
sqlite3_native__on_vfs_truncate(sqlite3_file *handle, sqlite_int64 size) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sync(sqlite3_file *handle, int flags) {
  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_size_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_size_t *data;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc >= 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_FSTAT);

  if (data->status == SQLITE_OK) {
    assert(argc == 2);

    err = js_get_value_int64(env, argv[1], &data->size);
    assert(err == 0);
  }

  uv_sem_post(&data->file->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_size_call(js_env_t *env, js_value_t *on_size, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_size_t *data = (sqlite3_native_size_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  js_value_t *args[2];

  err = js_create_uint32(env, data->file->type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_size_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_size, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_size(sqlite3_file *handle, sqlite_int64 *size) {
  int err;

  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  sqlite3_native_vfs_t *vfs = file->vfs;

  sqlite3_native_size_t data = {
    file,
  };

  err = js_call_threadsafe_function(vfs->on_size, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  if (data.status != SQLITE_OK) return data.status;

  *size = data.size;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_lock(sqlite3_file *handle, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_unlock(sqlite3_file *sql_file, int eLock) {
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_check_reserved_lock(sqlite3_file *sql_file, int *pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_control(sqlite3_file *sql_file, int op, void *pArg) {
  return SQLITE_NOTFOUND;
}

static int
sqlite3_native__on_vfs_sector_size(sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_device_characteristics(sqlite3_file *sql_file) {
  return 0;
}

static int
sqlite3_native__on_vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *handle, int flags, int *pflags) {
  sqlite3_native_file_t *file = (sqlite3_native_file_t *) handle;

  file->type = sqlite3_native__get_file_type(flags);

  if (file->type < 0) return SQLITE_CANTOPEN;

  file->vfs = (sqlite3_native_vfs_t *) vfs;

  static const sqlite3_io_methods methods = {
    1, // Version
    sqlite3_native__on_vfs_close,
    sqlite3_native__on_vfs_read,
    sqlite3_native__on_vfs_write,
    sqlite3_native__on_vfs_truncate,
    sqlite3_native__on_vfs_sync,
    sqlite3_native__on_vfs_size,
    sqlite3_native__on_vfs_lock,
    sqlite3_native__on_vfs_unlock,
    sqlite3_native__on_vfs_check_reserved_lock,
    sqlite3_native__on_vfs_control,
    sqlite3_native__on_vfs_sector_size,
    sqlite3_native__on_vfs_device_characteristics
  };

  file->handle.pMethods = &methods;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native__on_vfs_delete_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_delete_t *data;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc == 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_DELETE);

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_delete_call(js_env_t *env, js_value_t *on_delete, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_delete_t *data = (sqlite3_native_delete_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(data->name);

  js_value_t *args[2];

  err = js_create_uint32(env, type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_delete_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_delete, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_delete(sqlite3_vfs *handle, const char *name, int sync) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  sqlite3_native_delete_t data = {
    vfs,
    name,
    sync
  };

  err = js_call_threadsafe_function(vfs->on_delete, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  return data.status;
}

static js_value_t *
sqlite3_native__on_vfs_access_done(js_env_t *env, js_callback_info_t *info) {
  int err;

  sqlite3_native_access_t *data;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, (void **) &data);
  assert(err == 0);

  assert(argc >= 1);

  data->status = sqlite3_native__error_from(env, argv[0], SQLITE_IOERR_ACCESS);

  if (data->status == SQLITE_OK) {
    assert(argc == 2);

    err = js_get_value_bool(env, argv[1], &data->exists);
    assert(err == 0);
  }

  uv_sem_post(&data->vfs->done);

  return NULL;
}

static void
sqlite3_native__on_vfs_access_call(js_env_t *env, js_value_t *on_access, void *context, void *arg) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) context;

  sqlite3_native_access_t *data = (sqlite3_native_access_t *) arg;

  js_value_t *ctx;
  err = js_get_reference_value(env, vfs->ctx, &ctx);
  assert(err == 0);

  int type = sqlite3_native__get_file_type_from_name(data->name);

  js_value_t *args[2];

  err = js_create_uint32(env, type, &args[0]);
  assert(err == 0);

  err = js_create_function(env, "done", -1, sqlite3_native__on_vfs_access_done, (void *) data, &args[1]);
  assert(err == 0);

  err = js_call_function(env, ctx, on_access, 2, args, NULL);
  assert(err == 0);
}

static int
sqlite3_native__on_vfs_access(sqlite3_vfs *handle, const char *name, int flags, int *exists) {
  int err;

  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  sqlite3_native_access_t data = {
    vfs,
    name,
    flags
  };

  err = js_call_threadsafe_function(vfs->on_access, (void *) &data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&vfs->done);

  if (data.status != SQLITE_OK) return data.status;

  *exists = data.exists;

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_fullpathname(sqlite3_vfs *vfs, const char *name, int len, char *out) {
  if (strlen(name) >= len) return SQLITE_ERROR;

  strcpy(out, name);

  return SQLITE_OK;
}

static void *
sqlite3_native__on_vfs_dlopen(sqlite3_vfs *handle, const char *path) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  uv_lib_t *lib = malloc(sizeof(uv_lib_t));

  if (uv_dlopen(path, lib) == 0) return (void *) lib;

  snprintf(vfs->dlerror, sizeof(vfs->dlerror), "%s", uv_dlerror(lib));

  uv_dlclose(lib);

  free(lib);

  return NULL;
}

static void
sqlite3_native__on_vfs_dlerror(sqlite3_vfs *handle, int len, char *out) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  snprintf(out, (size_t) len, "%s", vfs->dlerror);
}

static sqlite3_native_dlsym_t
sqlite3_native__on_vfs_dlsym(sqlite3_vfs *handle, void *lib, const char *symbol) {
  sqlite3_native_vfs_t *vfs = (sqlite3_native_vfs_t *) handle;

  union {
    void *ptr;
    sqlite3_native_dlsym_t sym;
  } cast;

  if (uv_dlsym((uv_lib_t *) lib, symbol, &cast.ptr) == 0) return cast.sym;

  snprintf(vfs->dlerror, sizeof(vfs->dlerror), "%s", uv_dlerror((uv_lib_t *) lib));

  return NULL;
}

static void
sqlite3_native__on_vfs_dlclose(sqlite3_vfs *handle, void *lib) {
  uv_dlclose((uv_lib_t *) lib);

  free(lib);
}

static int
sqlite3_native__on_vfs_randomness(sqlite3_vfs *vfs, int bytes, char *buf) {
  memset(buf, 0, bytes);

  return SQLITE_OK;
}

static int
sqlite3_native__on_vfs_sleep(sqlite3_vfs *vfs, int nMicro) {
  return 0;
}

static int
sqlite3_native__on_vfs_current_time(sqlite3_vfs *vfs, double *time) {
  int err;

  uv_timespec64_t ts;
  err = uv_clock_gettime(UV_CLOCK_REALTIME, &ts);
  assert(err == 0);

  *time = ts.tv_sec / 86400.0 + 2440587.5;

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_vfs_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 6;
  js_value_t *argv[6];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 6);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  js_value_t *handle;

  sqlite3_native_vfs_t *vfs;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_vfs_t), (void **) &vfs, &handle);
  assert(err == 0);

  err = uv_sem_init(&vfs->done, 0);
  assert(err == 0);

  uv_random_t req;
  err = uv_random(loop, &req, vfs->name, sizeof(vfs->name), 0, NULL);
  assert(err == 0);

  vfs->name[sizeof(vfs->name) - 1] = '\0';

  vfs->env = env;

  err = js_create_reference(env, argv[0], 1, &vfs->ctx);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[1], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_access_call, &vfs->on_access);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[2], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_size_call, &vfs->on_size);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[3], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_read_call, &vfs->on_read);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[4], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_write_call, &vfs->on_write);
  assert(err == 0);

  err = js_create_threadsafe_function(env, argv[5], sqlite3_native__queue_limit, 1, NULL, NULL, (void *) vfs, sqlite3_native__on_vfs_delete_call, &vfs->on_delete);
  assert(err == 0);

  vfs->handle = (sqlite3_vfs) {
    1, // Version
    sizeof(sqlite3_native_file_t),
    sizeof(sqlite3_native_path_t),
    NULL,
    vfs->name,
    NULL,
    sqlite3_native__on_vfs_open,
    sqlite3_native__on_vfs_delete,
    sqlite3_native__on_vfs_access,
    sqlite3_native__on_vfs_fullpathname,
    sqlite3_native__on_vfs_dlopen,
    sqlite3_native__on_vfs_dlerror,
    sqlite3_native__on_vfs_dlsym,
    sqlite3_native__on_vfs_dlclose,
    sqlite3_native__on_vfs_randomness,
    sqlite3_native__on_vfs_sleep,
    sqlite3_native__on_vfs_current_time,
  };

  err = sqlite3_vfs_register(&vfs->handle, false);
  assert(err == 0);

  return handle;
}

static js_value_t *
sqlite3_native_vfs_destroy(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &vfs, NULL);
  assert(err == 0);

  uv_sem_destroy(&vfs->done);

  err = sqlite3_vfs_unregister(&vfs->handle);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_access, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_size, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_read, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_write, js_threadsafe_function_release);
  assert(err == 0);

  err = js_release_threadsafe_function(vfs->on_delete, js_threadsafe_function_release);
  assert(err == 0);

  err = js_delete_reference(env, vfs->ctx);
  assert(err == 0);

  return NULL;
}

static void
sqlite3_native__on_result_call(js_env_t *env, js_value_t *on_result, void *context, void *arg) {
  int err;

  sqlite3_native_t *db = (sqlite3_native_t *) context;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  js_value_t *result;
  err = js_get_reference_value(env, data->result, &result);
  assert(err == 0);

  js_value_t *rows;
  err = js_create_array_with_length(env, data->len, &rows);
  assert(err == 0);

  js_value_t *columns;
  err = js_create_array_with_length(env, data->len, &columns);
  assert(err == 0);

  for (int i = 0, n = data->len; i < n; i++) {
    js_value_t *row;

    if (data->rows[i] == NULL) {
      err = js_get_null(env, &row);
      assert(err == 0);
    } else {
      err = js_create_string_utf8(env, (const utf8_t *) data->rows[i], -1, &row);
      assert(err == 0);
    }

    err = js_set_element(env, rows, i, row);
    assert(err == 0);

    js_value_t *col;
    err = js_create_string_utf8(env, (const utf8_t *) data->columns[i], -1, &col);
    assert(err == 0);

    err = js_set_element(env, columns, i, col);
    assert(err == 0);
  }

  js_value_t *entry;
  err = js_create_object(env, &entry);
  assert(err == 0);

  err = js_set_named_property(env, entry, "rows", rows);
  assert(err == 0);

  err = js_set_named_property(env, entry, "columns", columns);
  assert(err == 0);

  err = js_set_element(env, result, data->i++, entry);
  assert(err == 0);

  uv_sem_post(&data->done);
}

static int
sqlite3_native__on_result(void *arg, int len, char **rows, char **columns) {
  int err;

  sqlite3_native_exec_t *data = (sqlite3_native_exec_t *) arg;

  data->len = len;
  data->rows = rows;
  data->columns = columns;

  err = js_call_threadsafe_function(data->db->on_result, (void *) data, js_threadsafe_function_blocking);
  assert(err == 0);

  uv_sem_wait(&data->done);

  return SQLITE_OK;
}

static js_value_t *
sqlite3_native_init(js_env_t *env, js_callback_info_t *info) {
  int err;

  js_value_t *handle;

  sqlite3_native_t *db;
  err = js_create_arraybuffer(env, sizeof(sqlite3_native_t), (void **) &db, &handle);
  assert(err == 0);

  db->env = env;

  err = js_create_threadsafe_function(env, NULL, sqlite3_native__queue_limit, 1, NULL, NULL, (void *) db, sqlite3_native__on_result_call, &db->on_result);
  assert(err == 0);

  return handle;
}

static void
sqlite3_native__on_after_open(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_open_t *req = (sqlite3_native_open_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;
  err = js_get_undefined(env, &result);
  assert(err == 0);

  err = js_resolve_deferred(env, req->deferred, result);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_open(uv_work_t *handle) {
  int err;

  sqlite3_native_open_t *req = (sqlite3_native_open_t *) handle->data;

  err = sqlite3_open_v2((char *) req->name, &req->db->handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, req->vfs->name);
  assert(err == 0);

  if (req->extensions) {
    err = sqlite3_enable_load_extension(req->db->handle, 1);
    assert(err == 0);
  }
}

static js_value_t *
sqlite3_native_open(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 4);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  sqlite3_native_vfs_t *vfs;
  err = js_get_arraybuffer_info(env, argv[1], (void **) &vfs, NULL);
  assert(err == 0);

  sqlite3_native_path_t name;
  err = js_get_value_string_utf8(env, argv[2], name, sizeof(name), NULL);
  assert(err == 0);

  bool extensions;
  err = js_get_value_bool(env, argv[3], &extensions);
  assert(err == 0);

  sqlite3_native_open_t *req = malloc(sizeof(sqlite3_native_open_t));

  req->db = db;
  req->vfs = vfs;
  req->extensions = extensions;

  memcpy(req->name, name, sizeof(name));

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_open, sqlite3_native__on_after_open);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_close(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_close_t *req = (sqlite3_native_close_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;
  err = js_get_undefined(env, &result);
  assert(err == 0);

  err = js_resolve_deferred(env, req->deferred, result);
  assert(err == 0);

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_release_threadsafe_function(db->on_result, js_threadsafe_function_release);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_close(uv_work_t *handle) {
  int err;

  sqlite3_native_close_t *req = (sqlite3_native_close_t *) handle->data;

  err = sqlite3_close_v2(req->db->handle);
  assert(err == 0);
}

static js_value_t *
sqlite3_native_close(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 1;
  js_value_t *argv[1];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 1);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  sqlite3_native_close_t *req = malloc(sizeof(sqlite3_native_close_t));

  req->db = db;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_close, sqlite3_native__on_after_close);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_exec(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_exec_t *req = (sqlite3_native_exec_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;

  if (req->error) {
    js_value_t *message;
    err = js_create_string_utf8(env, (utf8_t *) req->error, -1, &message);
    assert(err == 0);

    sqlite3_free(req->error);

    err = js_create_error(env, NULL, message, &result);
    assert(err == 0);

    err = js_reject_deferred(env, req->deferred, result);
    assert(err == 0);
  } else {
    err = js_get_reference_value(env, req->result, &result);
    assert(err == 0);

    err = js_resolve_deferred(env, req->deferred, result);
    assert(err == 0);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  err = js_delete_reference(env, req->result);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_exec(uv_work_t *handle) {
  int err;

  sqlite3_native_exec_t *req = (sqlite3_native_exec_t *) handle->data;

  err = uv_sem_init(&req->done, 0);
  assert(err == 0);

  sqlite3_exec(req->db->handle, (const char *) req->query, sqlite3_native__on_result, (void *) req, &req->error);

  free(req->query);

  uv_sem_destroy(&req->done);
}

static js_value_t *
sqlite3_native_exec(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 2;
  js_value_t *argv[2];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 2);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  size_t query_len;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &query_len);
  assert(err == 0);

  query_len += 1 /* NULL */;

  utf8_t *query = (utf8_t *) malloc(query_len);

  err = js_get_value_string_utf8(env, argv[1], query, query_len, NULL);
  assert(err == 0);

  js_value_t *result;
  err = js_create_array(env, &result);
  assert(err == 0);

  sqlite3_native_exec_t *req = malloc(sizeof(sqlite3_native_exec_t));

  req->db = db;
  req->query = query;
  req->i = 0;

  req->handle.data = (void *) req;

  err = js_create_reference(env, result, 1, &req->result);
  assert(err == 0);

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_exec, sqlite3_native__on_after_exec);
  assert(err == 0);

  return promise;
}

static char *
sqlite3_native__copy_string(const char *string, size_t len) {
  char *copy = malloc(len + 1);
  if (copy == NULL) return NULL;

  if (len > 0) memcpy(copy, string, len);
  copy[len] = '\0';

  return copy;
}

static char *
sqlite3_native__format_string(const char *format, ...) {
  va_list args;
  va_start(args, format);

  va_list copy;
  va_copy(copy, args);

  int len = vsnprintf(NULL, 0, format, copy);

  va_end(copy);

  if (len < 0) {
    va_end(args);
    return NULL;
  }

  char *string = malloc((size_t) len + 1);
  if (string == NULL) {
    va_end(args);
    return NULL;
  }

  vsnprintf(string, (size_t) len + 1, format, args);

  va_end(args);

  return string;
}

static js_value_t *
sqlite3_native__rejected_error(js_env_t *env, const char *message) {
  int err;

  js_deferred_t *deferred;
  js_value_t *promise;
  err = js_create_promise(env, &deferred, &promise);
  assert(err == 0);

  js_value_t *js_message;
  err = js_create_string_utf8(env, (const utf8_t *) message, -1, &js_message);
  assert(err == 0);

  js_value_t *error;
  err = js_create_error(env, NULL, js_message, &error);
  assert(err == 0);

  err = js_reject_deferred(env, deferred, error);
  assert(err == 0);

  return promise;
}

static void *
sqlite3_native__copy_bytes(const void *bytes, int len) {
  if (len == 0) return NULL;

  void *copy = malloc(len);
  if (copy == NULL) return NULL;

  memcpy(copy, bytes, len);

  return copy;
}

static void
sqlite3_native__set_query_error(sqlite3_native_query_t *req, const char *message) {
  if (req->error != NULL) return;

  req->error = sqlite3_native__copy_string(message, strlen(message));
}

static void
sqlite3_native__set_query_errorf(sqlite3_native_query_t *req, const char *format, ...) {
  if (req->error != NULL) return;

  va_list args;
  va_start(args, format);

  va_list copy;
  va_copy(copy, args);

  int len = vsnprintf(NULL, 0, format, copy);

  va_end(copy);

  if (len < 0) {
    va_end(args);
    sqlite3_native__set_query_error(req, "SQLite query failed");
    return;
  }

  req->error = malloc((size_t) len + 1);
  if (req->error == NULL) {
    va_end(args);
    return;
  }

  vsnprintf(req->error, (size_t) len + 1, format, args);

  va_end(args);
}

static void
sqlite3_native__set_query_sqlite_error(sqlite3_native_query_t *req) {
  const char *message = sqlite3_errmsg(req->db->handle);
  sqlite3_native__set_query_error(req, message == NULL ? "SQLite query failed" : message);
}

static void
sqlite3_native__free_query_value(sqlite3_native_query_value_t *value) {
  if (value->type == SQLITE3_NATIVE_VALUE_TEXT || value->type == SQLITE3_NATIVE_VALUE_BLOB) {
    free(value->bytes);
  }
}

static void
sqlite3_native__free_query_param(sqlite3_native_query_param_t *param) {
  if (param->type == SQLITE3_NATIVE_VALUE_TEXT || param->type == SQLITE3_NATIVE_VALUE_BLOB) {
    free(param->bytes);
  }
}

static void
sqlite3_native__free_query(sqlite3_native_query_t *req) {
  if (req == NULL) return;

  free(req->query);

  for (int i = 0; i < req->params_len; i++) {
    sqlite3_native__free_query_param(&req->params[i]);
  }
  free(req->params);

  for (int i = 0; i < req->column_count; i++) {
    free(req->column_names[i]);
  }
  free(req->column_names);

  for (int i = 0; i < req->row_count; i++) {
    sqlite3_native_query_row_t *row = &req->rows[i];
    for (int j = 0; j < req->column_count; j++) {
      sqlite3_native__free_query_value(&row->values[j]);
    }
    free(row->values);
  }
  free(req->rows);

  free(req->error);
  free(req);
}

static int
sqlite3_native__copy_js_param(js_env_t *env, js_value_t *value, sqlite3_native_query_param_t *param, uint32_t index, char **error) {
  int err;

  memset(param, 0, sizeof(sqlite3_native_query_param_t));

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  if (err != 0) return err;

  if (type == js_null) {
    param->type = SQLITE3_NATIVE_VALUE_NULL;
    return 0;
  }

  if (type == js_number) {
    double number;
    err = js_get_value_double(env, value, &number);
    if (err != 0) return err;

    if (!isfinite(number)) {
      *error = sqlite3_native__format_string("SQLite parameter %u must be a finite number", index);
      return -1;
    }

    if (
      number >= sqlite3_native__min_safe_integer &&
      number <= sqlite3_native__max_safe_integer &&
      number == (double) ((int64_t) number)
    ) {
      param->type = SQLITE3_NATIVE_VALUE_INTEGER;
      param->integer = (int64_t) number;
    } else {
      param->type = SQLITE3_NATIVE_VALUE_REAL;
      param->real = number;
    }

    return 0;
  }

  if (type == js_string) {
    size_t len;
    err = js_get_value_string_utf8(env, value, NULL, 0, &len);
    if (err != 0) return err;
    if (len > INT_MAX) {
      *error = sqlite3_native__format_string("SQLite parameter %u is too large", index);
      return -1;
    }

    char *copy = malloc(len + 1);
    if (copy == NULL) {
      *error = sqlite3_native__copy_string("Out of memory", strlen("Out of memory"));
      return -1;
    }

    err = js_get_value_string_utf8(env, value, (utf8_t *) copy, len + 1, NULL);
    if (err != 0) {
      free(copy);
      return err;
    }

    param->type = SQLITE3_NATIVE_VALUE_TEXT;
    param->bytes = copy;
    param->len = (int) len;

    return 0;
  }

  if (type == js_object) {
    bool is_typedarray;
    err = js_is_typedarray(env, value, &is_typedarray);
    if (err != 0) return err;

    if (is_typedarray) {
      js_typedarray_type_t typedarray_type;
      void *data;
      size_t len;

      err = js_get_typedarray_info(env, value, &typedarray_type, &data, &len, NULL, NULL);
      if (err != 0) return err;

      if (typedarray_type != js_uint8array) {
        *error = sqlite3_native__format_string("SQLite parameter %u must be a Uint8Array", index);
        return -1;
      }

      if (len > INT_MAX) {
        *error = sqlite3_native__format_string("SQLite parameter %u is too large", index);
        return -1;
      }

      void *copy = sqlite3_native__copy_bytes(data, (int) len);
      if (copy == NULL && len > 0) {
        *error = sqlite3_native__copy_string("Out of memory", strlen("Out of memory"));
        return -1;
      }

      param->type = SQLITE3_NATIVE_VALUE_BLOB;
      param->bytes = copy;
      param->len = (int) len;

      return 0;
    }
  }

  *error = sqlite3_native__format_string("Unsupported SQLite parameter at index %u", index);
  return -1;
}

static int
sqlite3_native__parse_query_mode(js_env_t *env, js_value_t *value, sqlite3_native_query_mode_t *mode, char **error) {
  int err;

  js_value_type_t type;
  err = js_typeof(env, value, &type);
  if (err != 0) return err;

  if (type != js_string) {
    *error = sqlite3_native__copy_string("SQLite query mode must be a string", strlen("SQLite query mode must be a string"));
    return -1;
  }

  char string[8];
  size_t len;
  err = js_get_value_string_utf8(env, value, (utf8_t *) string, sizeof(string), &len);
  if (err != 0) return err;

  if (len == 3 && strcmp(string, "run") == 0) {
    *mode = SQLITE3_NATIVE_QUERY_RUN;
    return 0;
  }
  if (len == 3 && strcmp(string, "all") == 0) {
    *mode = SQLITE3_NATIVE_QUERY_ALL;
    return 0;
  }
  if (len == 6 && strcmp(string, "values") == 0) {
    *mode = SQLITE3_NATIVE_QUERY_VALUES;
    return 0;
  }
  if (len == 3 && strcmp(string, "get") == 0) {
    *mode = SQLITE3_NATIVE_QUERY_GET;
    return 0;
  }

  *error = sqlite3_native__copy_string("SQLite query mode must be run, all, values, or get", strlen("SQLite query mode must be run, all, values, or get"));
  return -1;
}

static int
sqlite3_native__bind_query_param(sqlite3_stmt *stmt, int index, sqlite3_native_query_param_t *param) {
  switch (param->type) {
  case SQLITE3_NATIVE_VALUE_NULL:
    return sqlite3_bind_null(stmt, index);
  case SQLITE3_NATIVE_VALUE_INTEGER:
    return sqlite3_bind_int64(stmt, index, param->integer);
  case SQLITE3_NATIVE_VALUE_REAL:
    return sqlite3_bind_double(stmt, index, param->real);
  case SQLITE3_NATIVE_VALUE_TEXT:
    return sqlite3_bind_text(stmt, index, (const char *) param->bytes, param->len, SQLITE_TRANSIENT);
  case SQLITE3_NATIVE_VALUE_BLOB:
    return sqlite3_bind_blob(stmt, index, param->bytes, param->len, SQLITE_TRANSIENT);
  }

  return SQLITE_MISUSE;
}

static int
sqlite3_native__copy_column_names(sqlite3_native_query_t *req, sqlite3_stmt *stmt) {
  if (req->column_count == 0) return SQLITE_OK;

  req->column_names = calloc((size_t) req->column_count, sizeof(char *));
  if (req->column_names == NULL) return SQLITE_NOMEM;

  for (int i = 0; i < req->column_count; i++) {
    const char *name = sqlite3_column_name(stmt, i);
    if (name == NULL) name = "";

    req->column_names[i] = sqlite3_native__copy_string(name, strlen(name));
    if (req->column_names[i] == NULL) return SQLITE_NOMEM;
  }

  return SQLITE_OK;
}

static int
sqlite3_native__copy_column_value(sqlite3_stmt *stmt, int column, sqlite3_native_query_value_t *value) {
  memset(value, 0, sizeof(sqlite3_native_query_value_t));

  int type = sqlite3_column_type(stmt, column);

  switch (type) {
  case SQLITE_NULL:
    value->type = SQLITE3_NATIVE_VALUE_NULL;
    return SQLITE_OK;
  case SQLITE_INTEGER:
    value->type = SQLITE3_NATIVE_VALUE_INTEGER;
    value->integer = sqlite3_column_int64(stmt, column);
    return SQLITE_OK;
  case SQLITE_FLOAT:
    value->type = SQLITE3_NATIVE_VALUE_REAL;
    value->real = sqlite3_column_double(stmt, column);
    return SQLITE_OK;
  case SQLITE_TEXT: {
    int len = sqlite3_column_bytes(stmt, column);
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (text == NULL && len > 0) return SQLITE_NOMEM;

    char *copy = sqlite3_native__copy_string(text == NULL ? "" : (const char *) text, (size_t) len);
    if (copy == NULL) return SQLITE_NOMEM;

    value->type = SQLITE3_NATIVE_VALUE_TEXT;
    value->bytes = copy;
    value->len = len;
    return SQLITE_OK;
  }
  case SQLITE_BLOB: {
    int len = sqlite3_column_bytes(stmt, column);
    const void *blob = sqlite3_column_blob(stmt, column);
    if (blob == NULL && len > 0) return SQLITE_NOMEM;

    void *copy = sqlite3_native__copy_bytes(blob, len);
    if (copy == NULL && len > 0) return SQLITE_NOMEM;

    value->type = SQLITE3_NATIVE_VALUE_BLOB;
    value->bytes = copy;
    value->len = len;
    return SQLITE_OK;
  }
  }

  return SQLITE_MISUSE;
}

static int
sqlite3_native__append_query_row(sqlite3_native_query_t *req, sqlite3_stmt *stmt) {
  if (req->row_count == req->row_capacity) {
    int capacity = req->row_capacity == 0 ? 8 : req->row_capacity * 2;

    sqlite3_native_query_row_t *rows = realloc(req->rows, (size_t) capacity * sizeof(sqlite3_native_query_row_t));
    if (rows == NULL) return SQLITE_NOMEM;

    memset(rows + req->row_capacity, 0, (size_t) (capacity - req->row_capacity) * sizeof(sqlite3_native_query_row_t));

    req->rows = rows;
    req->row_capacity = capacity;
  }

  sqlite3_native_query_row_t *row = &req->rows[req->row_count];
  row->values = calloc((size_t) req->column_count, sizeof(sqlite3_native_query_value_t));
  if (row->values == NULL && req->column_count > 0) return SQLITE_NOMEM;

  for (int i = 0; i < req->column_count; i++) {
    int err = sqlite3_native__copy_column_value(stmt, i, &row->values[i]);
    if (err != SQLITE_OK) {
      for (int j = 0; j < i; j++) sqlite3_native__free_query_value(&row->values[j]);
      free(row->values);
      row->values = NULL;
      return err;
    }
  }

  req->row_count++;

  return SQLITE_OK;
}

static void
sqlite3_native__on_before_query(uv_work_t *handle) {
  sqlite3_native_query_t *req = (sqlite3_native_query_t *) handle->data;

  if (req->error != NULL) return;

  sqlite3_stmt *stmt = NULL;
  int err = sqlite3_prepare_v2(req->db->handle, (const char *) req->query, -1, &stmt, NULL);
  if (err != SQLITE_OK) {
    sqlite3_native__set_query_sqlite_error(req);
    return;
  }

  if (stmt == NULL) {
    sqlite3_native__set_query_error(req, "SQLite query did not contain a statement");
    return;
  }

  int bind_count = sqlite3_bind_parameter_count(stmt);
  if (bind_count != req->params_len) {
    sqlite3_native__set_query_errorf(req, "SQLite bind count mismatch: expected %d parameter(s), received %d", bind_count, req->params_len);
    goto done;
  }

  for (int i = 0; i < req->params_len; i++) {
    err = sqlite3_native__bind_query_param(stmt, i + 1, &req->params[i]);
    if (err != SQLITE_OK) {
      sqlite3_native__set_query_sqlite_error(req);
      goto done;
    }
  }

  if (req->mode == SQLITE3_NATIVE_QUERY_RUN) {
    do {
      err = sqlite3_step(stmt);
    } while (err == SQLITE_ROW);

    if (err != SQLITE_DONE) {
      sqlite3_native__set_query_sqlite_error(req);
      goto done;
    }

    req->changes = sqlite3_changes(req->db->handle);
    req->last_insert_rowid = sqlite3_last_insert_rowid(req->db->handle);
    goto done;
  }

  req->column_count = sqlite3_column_count(stmt);
  err = sqlite3_native__copy_column_names(req, stmt);
  if (err != SQLITE_OK) {
    sqlite3_native__set_query_error(req, "Out of memory");
    goto done;
  }

  while ((err = sqlite3_step(stmt)) == SQLITE_ROW) {
    err = sqlite3_native__append_query_row(req, stmt);
    if (err != SQLITE_OK) {
      sqlite3_native__set_query_error(req, "Out of memory");
      goto done;
    }

    if (req->mode == SQLITE3_NATIVE_QUERY_GET) goto done;
  }

  if (err != SQLITE_DONE) sqlite3_native__set_query_sqlite_error(req);

done:
  err = sqlite3_finalize(stmt);
  if (err != SQLITE_OK && req->error == NULL) sqlite3_native__set_query_sqlite_error(req);
}

static int
sqlite3_native__create_query_value(js_env_t *env, sqlite3_native_query_value_t *value, js_value_t **result) {
  int err;

  switch (value->type) {
  case SQLITE3_NATIVE_VALUE_NULL:
    return js_get_null(env, result);
  case SQLITE3_NATIVE_VALUE_INTEGER:
    return js_create_int64(env, value->integer, result);
  case SQLITE3_NATIVE_VALUE_REAL:
    return js_create_double(env, value->real, result);
  case SQLITE3_NATIVE_VALUE_TEXT:
    return js_create_string_utf8(env, (const utf8_t *) value->bytes, value->len, result);
  case SQLITE3_NATIVE_VALUE_BLOB: {
    js_value_t *arraybuffer;
    void *data;
    err = js_create_arraybuffer(env, (size_t) value->len, &data, &arraybuffer);
    if (err != 0) return err;

    if (value->len > 0) memcpy(data, value->bytes, (size_t) value->len);

    return js_create_typedarray(env, js_uint8array, (size_t) value->len, arraybuffer, 0, result);
  }
  }

  js_throw_error(env, NULL, "Unknown SQLite value type");
  return js_pending_exception;
}

static int
sqlite3_native__create_query_array_row(js_env_t *env, sqlite3_native_query_t *req, sqlite3_native_query_row_t *row, js_value_t **result) {
  int err;

  err = js_create_array_with_length(env, req->column_count, result);
  if (err != 0) return err;

  for (int i = 0; i < req->column_count; i++) {
    js_value_t *value;
    err = sqlite3_native__create_query_value(env, &row->values[i], &value);
    if (err != 0) return err;

    err = js_set_element(env, *result, i, value);
    if (err != 0) return err;
  }

  return 0;
}

static int
sqlite3_native__create_query_object_row(js_env_t *env, sqlite3_native_query_t *req, sqlite3_native_query_row_t *row, js_value_t **result) {
  int err;

  err = js_create_object(env, result);
  if (err != 0) return err;

  for (int i = 0; i < req->column_count; i++) {
    js_value_t *value;
    err = sqlite3_native__create_query_value(env, &row->values[i], &value);
    if (err != 0) return err;

    err = js_set_named_property(env, *result, req->column_names[i], value);
    if (err != 0) return err;
  }

  return 0;
}

static int
sqlite3_native__create_query_result(js_env_t *env, sqlite3_native_query_t *req, js_value_t **result) {
  int err;

  if (req->mode == SQLITE3_NATIVE_QUERY_RUN) {
    js_value_t *changes;
    js_value_t *last_insert_rowid;

    err = js_create_object(env, result);
    if (err != 0) return err;

    err = js_create_int64(env, req->changes, &changes);
    if (err != 0) return err;

    err = js_set_named_property(env, *result, "changes", changes);
    if (err != 0) return err;

    err = js_create_int64(env, req->last_insert_rowid, &last_insert_rowid);
    if (err != 0) return err;

    return js_set_named_property(env, *result, "lastInsertRowid", last_insert_rowid);
  }

  if (req->mode == SQLITE3_NATIVE_QUERY_GET) {
    if (req->row_count == 0) return js_get_null(env, result);

    return sqlite3_native__create_query_object_row(env, req, &req->rows[0], result);
  }

  err = js_create_array_with_length(env, req->row_count, result);
  if (err != 0) return err;

  for (int i = 0; i < req->row_count; i++) {
    js_value_t *row;
    if (req->mode == SQLITE3_NATIVE_QUERY_VALUES) {
      err = sqlite3_native__create_query_array_row(env, req, &req->rows[i], &row);
    } else {
      err = sqlite3_native__create_query_object_row(env, req, &req->rows[i], &row);
    }
    if (err != 0) return err;

    err = js_set_element(env, *result, i, row);
    if (err != 0) return err;
  }

  return 0;
}

static void
sqlite3_native__on_after_query(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_query_t *req = (sqlite3_native_query_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;

  if (req->error) {
    js_value_t *message;
    err = js_create_string_utf8(env, (utf8_t *) req->error, -1, &message);
    assert(err == 0);

    err = js_create_error(env, NULL, message, &result);
    assert(err == 0);

    err = js_reject_deferred(env, req->deferred, result);
    assert(err == 0);
  } else {
    err = sqlite3_native__create_query_result(env, req, &result);
    assert(err == 0);

    err = js_resolve_deferred(env, req->deferred, result);
    assert(err == 0);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  sqlite3_native__free_query(req);
}

static js_value_t *
sqlite3_native__queue_query_error(js_env_t *env, uv_loop_t *loop, sqlite3_native_t *db, const char *message) {
  int err;

  sqlite3_native_query_t *req = calloc(1, sizeof(sqlite3_native_query_t));
  if (req == NULL) return sqlite3_native__rejected_error(env, "Out of memory");

  req->db = db;
  req->error = sqlite3_native__copy_string(message, strlen(message));
  if (req->error == NULL) {
    free(req);
    return sqlite3_native__rejected_error(env, "Out of memory");
  }

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_query, sqlite3_native__on_after_query);
  assert(err == 0);

  return promise;
}

static js_value_t *
sqlite3_native_query(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 4;
  js_value_t *argv[4];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 4);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  char *validation_error = NULL;

  js_value_type_t query_type;
  err = js_typeof(env, argv[1], &query_type);
  if (err != 0) return NULL;
  if (query_type != js_string) {
    return sqlite3_native__queue_query_error(env, loop, db, "SQLite query SQL must be a string");
  }

  bool params_is_array;
  err = js_is_array(env, argv[2], &params_is_array);
  if (err != 0) return NULL;
  if (!params_is_array) {
    return sqlite3_native__queue_query_error(env, loop, db, "SQLite query params must be an array");
  }

  sqlite3_native_query_mode_t mode;
  err = sqlite3_native__parse_query_mode(env, argv[3], &mode, &validation_error);
  if (err != 0) {
    js_value_t *promise = sqlite3_native__queue_query_error(env, loop, db, validation_error == NULL ? "Invalid SQLite query mode" : validation_error);
    free(validation_error);
    return promise;
  }

  size_t query_len;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &query_len);
  if (err != 0) return NULL;

  utf8_t *query = malloc(query_len + 1);
  if (query == NULL) {
    return sqlite3_native__queue_query_error(env, loop, db, "Out of memory");
  }

  err = js_get_value_string_utf8(env, argv[1], query, query_len + 1, NULL);
  if (err != 0) {
    free(query);
    return NULL;
  }

  uint32_t params_len;
  err = js_get_array_length(env, argv[2], &params_len);
  if (err != 0) {
    free(query);
    return NULL;
  }
  if (params_len > INT_MAX) {
    free(query);
    return sqlite3_native__queue_query_error(env, loop, db, "SQLite query params array is too large");
  }

  sqlite3_native_query_param_t *params = NULL;
  if (params_len > 0) {
    params = calloc(params_len, sizeof(sqlite3_native_query_param_t));
    if (params == NULL) {
      free(query);
      return sqlite3_native__queue_query_error(env, loop, db, "Out of memory");
    }
  }

  for (uint32_t i = 0; i < params_len; i++) {
    js_value_t *value;
    err = js_get_element(env, argv[2], i, &value);
    if (err != 0) {
      free(query);
      for (uint32_t j = 0; j < i; j++) sqlite3_native__free_query_param(&params[j]);
      free(params);
      return NULL;
    }

    err = sqlite3_native__copy_js_param(env, value, &params[i], i, &validation_error);
    if (err != 0) {
      js_value_t *promise = sqlite3_native__queue_query_error(env, loop, db, validation_error == NULL ? "Invalid SQLite parameter" : validation_error);
      free(query);
      for (uint32_t j = 0; j <= i; j++) sqlite3_native__free_query_param(&params[j]);
      free(params);
      free(validation_error);
      return promise;
    }
  }

  sqlite3_native_query_t *req = calloc(1, sizeof(sqlite3_native_query_t));
  if (req == NULL) {
    free(query);
    for (uint32_t i = 0; i < params_len; i++) sqlite3_native__free_query_param(&params[i]);
    free(params);
    return sqlite3_native__queue_query_error(env, loop, db, "Out of memory");
  }

  req->db = db;
  req->query = query;
  req->params = params;
  req->params_len = (int) params_len;
  req->mode = mode;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_query, sqlite3_native__on_after_query);
  assert(err == 0);

  return promise;
}

static void
sqlite3_native__on_after_load_extension(uv_work_t *handle, int status) {
  int err;

  sqlite3_native_load_extension_t *req = (sqlite3_native_load_extension_t *) handle->data;

  sqlite3_native_t *db = req->db;

  js_env_t *env = db->env;

  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);

  js_value_t *result;

  if (req->error) {
    js_value_t *message;
    err = js_create_string_utf8(env, (utf8_t *) req->error, -1, &message);
    assert(err == 0);

    sqlite3_free(req->error);

    err = js_create_error(env, NULL, message, &result);
    assert(err == 0);

    err = js_reject_deferred(env, req->deferred, result);
    assert(err == 0);
  } else {
    err = js_get_undefined(env, &result);
    assert(err == 0);

    err = js_resolve_deferred(env, req->deferred, result);
    assert(err == 0);
  }

  err = js_close_handle_scope(env, scope);
  assert(err == 0);

  free(req);
}

static void
sqlite3_native__on_before_load_extension(uv_work_t *handle) {
  sqlite3_native_load_extension_t *req = (sqlite3_native_load_extension_t *) handle->data;

  sqlite3_load_extension(req->db->handle, (const char *) req->path, (const char *) req->entry, &req->error);

  free(req->path);
  free(req->entry);
}

static js_value_t *
sqlite3_native_load_extension(js_env_t *env, js_callback_info_t *info) {
  int err;

  size_t argc = 3;
  js_value_t *argv[3];

  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);

  assert(argc == 3);

  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);

  sqlite3_native_t *db;
  err = js_get_arraybuffer_info(env, argv[0], (void **) &db, NULL);
  assert(err == 0);

  size_t path_len;
  err = js_get_value_string_utf8(env, argv[1], NULL, 0, &path_len);
  assert(err == 0);

  path_len += 1 /* NULL */;

  utf8_t *path = (utf8_t *) malloc(path_len);

  err = js_get_value_string_utf8(env, argv[1], path, path_len, NULL);
  assert(err == 0);

  utf8_t *entry = NULL;

  js_value_type_t entry_type;
  err = js_typeof(env, argv[2], &entry_type);
  assert(err == 0);

  if (entry_type == js_string) {
    size_t entry_len;
    err = js_get_value_string_utf8(env, argv[2], NULL, 0, &entry_len);
    assert(err == 0);

    entry_len += 1 /* NULL */;

    entry = (utf8_t *) malloc(entry_len);

    err = js_get_value_string_utf8(env, argv[2], entry, entry_len, NULL);
    assert(err == 0);
  }

  sqlite3_native_load_extension_t *req = malloc(sizeof(sqlite3_native_load_extension_t));

  req->db = db;
  req->path = path;
  req->entry = entry;
  req->error = NULL;

  req->handle.data = (void *) req;

  js_value_t *promise;
  err = js_create_promise(env, &req->deferred, &promise);
  assert(err == 0);

  err = uv_queue_work(loop, &req->handle, sqlite3_native__on_before_load_extension, sqlite3_native__on_after_load_extension);
  assert(err == 0);

  return promise;
}

static js_value_t *
sqlite3_native_exports(js_env_t *env, js_value_t *exports) {
  int err;

#define V(name, fn) \
  { \
    js_value_t *val; \
    err = js_create_function(env, name, -1, fn, NULL, &val); \
    assert(err == 0); \
    err = js_set_named_property(env, exports, name, val); \
    assert(err == 0); \
  }

  V("vfsInit", sqlite3_native_vfs_init)
  V("vfsDestroy", sqlite3_native_vfs_destroy)

  V("init", sqlite3_native_init)
  V("open", sqlite3_native_open)
  V("close", sqlite3_native_close)
  V("exec", sqlite3_native_exec)
  V("loadExtension", sqlite3_native_load_extension)
  V("query", sqlite3_native_query)
#undef V

  return exports;
}

BARE_MODULE(sqlite3_native, sqlite3_native_exports)
