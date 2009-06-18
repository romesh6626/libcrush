#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <openssl/md5.h>

#include "fcgiapp.h"

#include "s3access.h"

#include <map>
#include <string>
#include <vector>
#include <iostream>

using namespace std;

static FILE *dbg;

struct s3_err {
  const char *code;
  const char *message;
};

struct req_state {
   FCGX_ParamArray envp;
   FCGX_Stream *in;
   FCGX_Stream *out;
   bool content_started;
   int indent;
   const char *path_name;
   const char *host;
   const char *method;
   const char *query;
   const char *length;
   struct s3_err err;
};


class NameVal
{
   string str;
   string name;
   string val;
 public:
    NameVal(string nv) : str(nv) {}

    int parse();

    string& get_name() { return name; }
    string& get_val() { return val; }
};

int NameVal::parse()
{
  int delim_pos = str.find('=');

  if (delim_pos < 0)
    return -1;

  name = str.substr(0, delim_pos);
  val = str.substr(delim_pos + 1);

  cout << "parsed: name=" << name << " val=" << val << std::endl;
  return 0; 
}

class XMLArgs
{
  string str, empty_str;
  map<string, string> val_map;
 public:
   XMLArgs(string s) : str(s) {}
   int parse();
   string& get(string& name);
   string& get(const char *name);
};

int XMLArgs::parse()
{
  int pos = 0, fpos;
  bool end = false;
  if (str[pos] == '?') pos++;

  while (!end) {
    fpos = str.find('&', pos);
    if (fpos  < pos) {
       end = true;
       fpos = str.size(); 
    }
    NameVal nv(str.substr(pos, fpos - pos));
    if (nv.parse() >= 0) {
      val_map[nv.get_name()] = nv.get_val();
    }

    pos = fpos + 1;  
  }

  return 0;
}

string& XMLArgs::get(string& name)
{
  map<string, string>::iterator iter;
  iter = val_map.find(name);
  if (iter == val_map.end())
    return empty_str;
  return iter->second;
}

string& XMLArgs::get(const char *name)
{
  string s(name);
  return get(s);
}

static void init_state(struct req_state *s, FCGX_ParamArray envp, FCGX_Stream *in, FCGX_Stream *out)
{
  s->envp = envp;
  s->in = in;
  s->out = out;
  s->content_started = false;
  s->indent = 0;
  s->path_name = FCGX_GetParam("SCRIPT_NAME", envp);
  s->method = FCGX_GetParam("REQUEST_METHOD", envp);
  s->host = FCGX_GetParam("HTTP_HOST", envp);
  s->query = FCGX_GetParam("QUERY_STRING", envp);
  s->length = FCGX_GetParam("CONTENT_LENGTH", envp);
  s->err.code = NULL;
  s->err.message = NULL;
}

static void buf_to_hex(const unsigned char *buf, int len, char *str)
{
  int i;
  str[0] = '\0';
  for (i = 0; i < len; i++) {
    sprintf(&str[i*2], "%02x", (int)buf[i]);
  }
}

static void dump_status(struct req_state *s, const char *status)
{
  FCGX_FPrintF(s->out,"Status: %s\n", status);
}
struct errno_http {
  int err;
  const char *http_str;
};

static struct errno_http hterrs[] = {
    { 0, "200" },
    { EINVAL, "400" },
    { EPERM, "401" },
    { EACCES, "403" },
    { ENOENT, "404" },
    { ETIMEDOUT, "408" },
    { EEXIST, "409" },
    { ENOTEMPTY, "409" },
    { 0, NULL }};

static void dump_errno(struct req_state *s, int err, const char *code = NULL, const char *message = NULL)
{
  const char *err_str = "500";

  if (err < 0)
    err = -err;

  int i=0;
  while (hterrs[i].http_str) {
    if (err == hterrs[i].err) {
      err_str = hterrs[i].http_str;
      break;
    }

    i++;
  }

  dump_status(s, err_str);
  if (err) {
    struct s3_err e;
    e.code = code;
    e.message = message;
    s->err = e;
  }
}

static void open_section(struct req_state *s, const char *name)
{
  FCGX_FPrintF(s->out, "%*s<%s>\n", s->indent, "", name);
  ++s->indent;
}

static void close_section(struct req_state *s, const char *name)
{
  --s->indent;
  FCGX_FPrintF(s->out, "%*s</%s>\n", s->indent, "", name);
}

static void dump_value(struct req_state *s, const char *name, const char *fmt, ...)
{
#define LARGE_SIZE 8192
  char buf[LARGE_SIZE];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, LARGE_SIZE, fmt, ap);
  va_end(ap);
  if (n >= LARGE_SIZE)
    return;
  FCGX_FPrintF(s->out, "%*s<%s>%s</%s>\n", s->indent, "", name, buf, name);
}

static void dump_entry(struct req_state *s, const char *val)
{
  FCGX_FPrintF(s->out, "%*s<?%s?>\n", s->indent, "", val);
}


static void dump_time(struct req_state *s, const char *name, time_t *t)
{
#define TIME_BUF_SIZE 128
  char buf[TIME_BUF_SIZE];
  struct tm *tmp = localtime(t);
  if (tmp == NULL)
    return;

  if (strftime(buf, sizeof(buf), "%Y-%m-%dT%T.000Z", tmp) == 0)
    return;

  dump_value(s, name, buf); 
}

static void dump_owner(struct req_state *s, const char *id, int id_size, const char *name)
{
  char id_str[id_size*2 + 1];

  buf_to_hex((const unsigned char *)id, id_size, id_str);

  open_section(s, "Owner");
  dump_value(s, "ID", id_str);
  dump_value(s, "DisplayName", name);
  close_section(s, "Owner");
}

static void dump_start_xml(struct req_state *s)
{
  if (!s->content_started) {
    dump_entry(s, "xml version=\"1.0\" encoding=\"UTF-8\"");
    s->content_started = true;
  }
}

static void end_header(struct req_state *s)
{
  FCGX_FPrintF(s->out,"Content-type: text/plain\r\n\r\n");
  if (s->err.code || s->err.message) {
    dump_start_xml(s);
    struct s3_err &err = s->err;
    open_section(s, "Error");
    if (err.code)
      dump_value(s, "Code", err.code);
    if (err.message)
      dump_value(s, "Message", err.message);
    close_section(s, "Error");
  }
}

static void list_all_buckets_start(struct req_state *s)
{
  open_section(s, "ListAllMyBucketsResult");
}

static void list_all_buckets_end(struct req_state *s)
{
  close_section(s, "ListAllMyBucketsResult");
}

static void dump_bucket(struct req_state *s, S3ObjEnt& obj)
{
  open_section(s, "Bucket");
  dump_value(s, "Name", obj.name.c_str());
  dump_time(s, "CreationDate", &obj.mtime);
  close_section(s, "Bucket");
}

void do_list_buckets(struct req_state *s)
{
  string id = "0123456789ABCDEF";
  S3AccessHandle handle;
  int r;
  S3ObjEnt obj;

  r = list_buckets_init(id, &handle);
  dump_errno(s, (r < 0 ? r : 0));
  end_header(s);
  dump_start_xml(s);

  list_all_buckets_start(s);
  dump_owner(s, (const char *)id.c_str(), id.size(), "foobi");

  open_section(s, "Buckets");
  while (r >= 0) {
    r = list_buckets_next(id, obj, &handle);
    if (r < 0)
      continue;
    dump_bucket(s, obj);
  }
  close_section(s, "Buckets");
  list_all_buckets_end(s);
}


void do_list_objects(struct req_state *s)
{
  int pos;
  string bucket, host_str;
  string prefix, marker, max_keys, delimiter;
  int max;
  const char *p;
  string id = "0123456789ABCDEF";

  if (s->path_name[0] == '/') {
    string tmp = s->path_name;
    bucket = tmp.substr(1);
    p = s->query;
  } else if (s->path_name [0] == '?') {
    if (!s->host)
      return;
    host_str = s->host;
    pos = host_str.find('.');
    if (pos >= 0) {
      bucket = host_str.substr(pos);
    } else {
      bucket = host_str;
    }
    p = s->path_name;
  } else {
    return;
  }

  XMLArgs args(p);
  args.parse();

  prefix = args.get("prefix");
  marker = args.get("marker");
  max_keys = args.get("max-keys");
 if (!max_keys.empty()) {
    max = atoi(max_keys.c_str());
  } else {
    max = -1;
  }
  delimiter = args.get("delimiter");

  vector<S3ObjEnt> objs;
  int r = list_objects(id, bucket, max, prefix, marker, objs);
  dump_errno(s, (r < 0 ? r : 0));

  end_header(s);
  dump_start_xml(s);
  if (r < 0)
    return;

  open_section(s, "ListBucketResult");
  dump_value(s, "Name", bucket.c_str()); 
  if (!prefix.empty())
    dump_value(s, "Prefix", prefix.c_str());
  if (!marker.empty())
    dump_value(s, "Marker", marker.c_str());
  if (!max_keys.empty()) {
    dump_value(s, "MaxKeys", max_keys.c_str());
  }
  if (!delimiter.empty())
    dump_value(s, "Delimiter", delimiter.c_str());

  if (r >= 0) {
    vector<S3ObjEnt>::iterator iter;
    for (iter = objs.begin(); iter != objs.end(); ++iter) {
      open_section(s, "Contents");
      dump_value(s, "Key", iter->name.c_str());
      dump_time(s, "LastModified", &iter->mtime);
      dump_value(s, "ETag", "&quot;828ef3fdfa96f00ad9f27c383fc9ac7f&quot;");
      dump_value(s, "Size", "%lld", iter->size);
      dump_value(s, "StorageClass", "STANDARD");
      dump_owner(s, (const char *)&id, sizeof(id), "foobi");
      close_section(s, "Contents");
    }
  }
  close_section(s, "ListBucketResult");
  //if (args.get("name"))
}

void do_create_bucket(struct req_state *s)
{
  const char *req_name = s->path_name + 1;
  string str(req_name);
  int pos = str.find('/');
  if (pos <= 0)
    pos = str.size();
  string bucket_name = str.substr(0, pos);
  string id = "0123456789ABCDEF";

  int r = create_bucket(id, bucket_name);

  dump_errno(s, r);
  end_header(s);
}

void do_create_object(struct req_state *s)
{
  const char *req_name = s->path_name + 1;
  int r = -EINVAL;
  string str(req_name);
  int pos = str.find('/');
  char *data = NULL;
  const char *err_code = NULL;
  const char *err_message = NULL;
  if (pos < 0) {
    goto done;
  } else {
    string bucket_name = str.substr(0, pos);
    string obj_name = str.substr(pos + 1);
    string id = "0123456789ABCDEF";

    size_t cl = atoll(s->length);
    size_t actual = 0;
    if (cl) {
      data = (char *)malloc(cl);
      if (!data) {
         r = -ENOMEM;
         goto done;
      }
      actual = FCGX_GetStr(data, cl, s->in);
    }

    char *supplied_md5 = FCGX_GetParam("HTTP_CONTENT_MD5", s->envp);
    char calc_md5[MD5_DIGEST_LENGTH];
    MD5_CTX c;
    unsigned char m[MD5_DIGEST_LENGTH];

    if (supplied_md5 && strlen(supplied_md5) != MD5_DIGEST_LENGTH*2) {
      err_code = "InvalidDigest";
      r = -EINVAL;
      goto done;
    }

    MD5_Init(&c);
    MD5_Update(&c, data, (unsigned long)actual);
    MD5_Final(m, &c);

    buf_to_hex(m, MD5_DIGEST_LENGTH, calc_md5);

    if (supplied_md5 && strcmp(calc_md5, supplied_md5)) {
       err_code = "BadDigest";
       r = -EINVAL;
       goto done;
    }

    string md5_str(calc_md5);
    r = put_obj(id, bucket_name, obj_name, data, actual, md5_str);
  }
done:
  free(data);
  dump_errno(s, r, err_code, err_message);

  end_header(s);
}

void do_delete_bucket(struct req_state *s)
{
  const char *req_name = s->path_name + 1;
  string str(req_name);
  int pos = str.find('/');
  if (pos <= 0)
    pos = str.size();
  string bucket_name = str.substr(0, pos);
  string id = "0123456789ABCDEF";

  int r = delete_bucket(id, bucket_name);

  dump_errno(s, r);
  end_header(s);
}

void do_delete_object(struct req_state *s)
{
  const char *req_name = s->path_name + 1;
  int r = -EINVAL;
  string str(req_name);
  int pos = str.find('/');
  if (pos < 0) {
    goto done;
  } else {
    string bucket_name = str.substr(0, pos);
    string obj_name = str.substr(pos + 1);
    string id = "0123456789ABCDEF";

    r = delete_obj(id, bucket_name, obj_name);
  }
done:
  dump_errno(s, r);

  end_header(s);
}

void do_retrieve(struct req_state *s)
{
  if (strcmp(s->path_name, "/") == 0)
    do_list_buckets(s);
  else
    do_list_objects(s);
}

void do_create(struct req_state *s)
{
  const char *p;
  bool create_bucket = false;
  if (!s->path_name)
    return;

  if (s->path_name[0] != '/')
    return;

  p = strchr(&s->path_name[1], '/');
  if (p) {
    if (*(p+1) == '\0')
      create_bucket = true;
  } else {
    create_bucket = true;
  }

  if (create_bucket)
    do_create_bucket(s);
  else
    do_create_object(s);
}

void do_delete(struct req_state *s)
{
  const char *p;
  bool delete_bucket = false;
  if (!s->path_name)
    return;

  if (s->path_name[0] != '/')
    return;

  p = strchr(&s->path_name[1], '/');
  if (p) {
    if (*(p+1) == '\0')
      delete_bucket = true;
  } else {
    delete_bucket = true;
  }

  if (delete_bucket)
    do_delete_bucket(s);
  else
    do_delete_object(s);
}


int main(void)
{
  FCGX_Stream *in, *out, *err;
  FCGX_ParamArray envp;
  struct req_state s;
  dbg = fopen("/tmp/fcgi.out", "a");

  while (FCGX_Accept(&in, &out, &err, &envp) >= 0) 
  {
    init_state(&s, envp, in, out);
    static int i=0;

    fprintf(dbg, "%d %s\n", i++, s.method);

    if (!s.method)
      continue;

    if (strcmp(s.method, "GET") == 0)
      do_retrieve(&s);
    if (strcmp(s.method, "PUT") == 0)
      do_create(&s);
    if (strcmp(s.method, "DELETE") == 0)
      do_delete(&s);
  }
  return 0;
}

