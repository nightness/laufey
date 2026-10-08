// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"

#include <cmath>

namespace laufey_common {

namespace {

// Frees a value fetched from the options on scope exit.
struct Fetched {
  const laufey_backend_api_t* api;
  laufey_value_t* v;
  ~Fetched() {
    if (v)
      api->value_free(v);
  }
};

std::string ReadDictString(const laufey_backend_api_t* api, laufey_value_t* dict,
                           const char* key, bool* present = nullptr) {
  Fetched f{api, api->value_dict_get(dict, key)};
  if (present)
    *present = false;
  if (!f.v || !api->value_is_string(f.v))
    return std::string();
  size_t len = 0;
  char* s = api->value_get_string(f.v, &len);
  if (!s)
    return std::string();
  std::string out(s, len);
  api->value_free_string(s);
  if (present)
    *present = true;
  return out;
}

bool ReadDictBool(const laufey_backend_api_t* api, laufey_value_t* dict,
                  const char* key, bool dfl) {
  Fetched f{api, api->value_dict_get(dict, key)};
  if (!f.v || !api->value_is_bool(f.v))
    return dfl;
  return api->value_get_bool(f.v);
}

// A number (int or double) as int64, or `dfl`. Non-finite values read as
// `dfl`.
int64_t ReadDictInt64(const laufey_backend_api_t* api, laufey_value_t* dict,
                      const char* key, int64_t dfl) {
  Fetched f{api, api->value_dict_get(dict, key)};
  if (!f.v)
    return dfl;
  if (api->value_is_int(f.v))
    return api->value_get_int(f.v);
  if (api->value_is_double(f.v)) {
    double d = api->value_get_double(f.v);
    if (!std::isfinite(d) || d < 0 || d > 9.0e15)
      return dfl;
    return static_cast<int64_t>(d);
  }
  return dfl;
}

}  // namespace

NotificationOptions ParseNotificationOptions(laufey_value_t* options,
                                             const laufey_backend_api_t* api) {
  NotificationOptions opts;
  if (!options)
    return opts;
  if (!api->value_is_dict(options)) {
    api->value_free(options);
    return opts;
  }

  opts.title = ReadDictString(api, options, "title");
  opts.body = ReadDictString(api, options, "body");
  opts.tag = ReadDictString(api, options, "tag");
  opts.silent = ReadDictBool(api, options, "silent", false);
  opts.require_interaction =
      ReadDictBool(api, options, "require_interaction", false);
  opts.schedule_at_ms = ReadDictInt64(api, options, "schedule_at", 0);
  opts.data = ReadDictString(api, options, "data", &opts.has_data);

  {
    Fetched actions{api, api->value_dict_get(options, "actions")};
    if (actions.v && api->value_is_list(actions.v)) {
      size_t n = api->value_list_size(actions.v);
      opts.actions.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        Fetched a{api, api->value_list_get(actions.v, i)};
        if (!a.v || !api->value_is_dict(a.v))
          continue;
        NotificationAction act;
        act.id = ReadDictString(api, a.v, "id");
        act.title = ReadDictString(api, a.v, "title");
        if (!act.id.empty() && !act.title.empty())
          opts.actions.push_back(std::move(act));
      }
    }
  }

  {
    Fetched icon{api, api->value_dict_get(options, "icon")};
    if (icon.v && api->value_is_binary(icon.v)) {
      size_t len = 0;
      const void* ptr = api->value_get_binary(icon.v, &len);
      if (ptr && len > 0) {
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        opts.icon_png.assign(p, p + len);
      }
    }
  }

  api->value_free(options);
  return opts;
}

}  // namespace laufey_common
