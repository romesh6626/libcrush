#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/md5.h>

#include "fcgiapp.h"

#include "s3access.h"
#include "s3acl.h"
#include "user.h"
#include "s3op.h"
#include "s3rest.h"

#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include "include/types.h"
#include "include/base64.h"
#include "common/BackTrace.h"

using namespace std;


#define CGI_PRINTF(stream, format, ...) do { \
   fprintf(stderr, format, __VA_ARGS__); \
   FCGX_FPrintF(stream, format, __VA_ARGS__); \
} while (0)

void init_entities_from_header(struct req_state *s)
{
  s->bucket = NULL;
  s->bucket_str = "";
  s->object = NULL;
  s->object_str = "";

  string h(s->host);

  cerr << "host=" << s->host << std::endl;
  int pos = h.find("s3.");

  if (pos > 0) {
    s->bucket_str = h.substr(0, pos-1);
    s->bucket = s->bucket_str.c_str();
    s->host_bucket = s->bucket;
  } else {
    s->host_bucket = NULL;
  }

  const char *req_name = s->path_name;
  const char *p;

  if (*req_name == '?') {
    p = req_name;
  } else {
    p = s->query;
  }

  s->args.set(p);
  s->args.parse();

  if (*req_name != '/')
    return;

  req_name++;

  if (!*req_name)
    return;

  string req(req_name);
  string first;

  pos = req.find('/');
  if (pos >= 0) {
    first = req.substr(0, pos);
  } else {
    first = req;
  }

  if (!s->bucket) {
    s->bucket_str = first;
    s->bucket = s->bucket_str.c_str();
  } else {
    s->object_str = req;
    s->object = s->object_str.c_str();
    return;
  }

  if (pos >= 0) {
    s->object_str = req.substr(pos+1);

    if (s->object_str.size() > 0) {
      s->object = s->object_str.c_str();
    }
  }
}

static void line_unfold(const char *line, string& sdest)
{
  char dest[strlen(line) + 1];
  const char *p = line;
  char *d = dest;

  while (isspace(*p))
    ++p;

  bool last_space = false;

  while (*p) {
    switch (*p) {
    case '\n':
    case '\r':
      *d = ' ';
      if (!last_space)
        ++d;
      last_space = true;
      break;
    default:
      *d = *p;
      ++d;
      last_space = false;
      break;
    }
    ++p;
  }
  *d = 0;
  sdest = dest;
}

static void init_auth_info(struct req_state *s)
{
  const char *p;

  s->x_amz_map.clear();

  for (int i=0; (p = s->fcgx->envp[i]); ++i) {
#define HTTP_X_AMZ "HTTP_X_AMZ"
    if (strncmp(p, HTTP_X_AMZ, sizeof(HTTP_X_AMZ) - 1) == 0) {
      cerr << "amz>> " << p << std::endl;
      const char *amz = p+5; /* skip the HTTP_ part */
      const char *eq = strchr(amz, '=');
      if (!eq) /* shouldn't happen! */
        continue;
      int len = eq - amz;
      char amz_low[len + 1];
      int j;
      for (j=0; j<len; j++) {
        amz_low[j] = tolower(amz[j]);
        if (amz_low[j] == '_')
          amz_low[j] = '-';
      }
      amz_low[j] = 0;
      string val;
      line_unfold(eq + 1, val);

      map<string, string>::iterator iter;
      iter = s->x_amz_map.find(amz_low);
      if (iter != s->x_amz_map.end()) {
        string old = iter->second;
        int pos = old.find_last_not_of(" \t"); /* get rid of any whitespaces after the value */
        old = old.substr(0, pos + 1);
        old.append(",");
        old.append(val);
        s->x_amz_map[amz_low] = old;
      } else {
        s->x_amz_map[amz_low] = val;
      }
    }
  }
  map<string, string>::iterator iter;
  for (iter = s->x_amz_map.begin(); iter != s->x_amz_map.end(); ++iter) {
    cerr << "x>> " << iter->first << ":" << iter->second << std::endl;
  }
  
}

static void get_request_metadata(struct req_state *s, map<nstring, bufferlist>& attrs)
{
  map<string, string>::iterator iter;
  for (iter = s->x_amz_map.begin(); iter != s->x_amz_map.end(); ++iter) {
    string name = iter->first;
#define X_AMZ_META "x-amz-meta"
    if (name.find(X_AMZ_META) == 0) {
      cerr << "x>> " << iter->first << ":" << iter->second << std::endl;
      string& val = iter->second;
      bufferlist bl;
      bl.append(val.c_str(), val.size() + 1);
      string attr_name = S3_ATTR_PREFIX;
      attr_name.append(name);
      attrs[attr_name.c_str()] = bl;
    }
  }
}

static void get_canon_amz_hdr(struct req_state *s, string& dest)
{
  dest = "";
  map<string, string>::iterator iter;
  for (iter = s->x_amz_map.begin(); iter != s->x_amz_map.end(); ++iter) {
    dest.append(iter->first);
    dest.append(":");
    dest.append(iter->second);
    dest.append("\n");
  }
}

static void get_canon_resource(struct req_state *s, string& dest)
{
  if (s->host_bucket) {
    dest = "/";
    dest.append(s->host_bucket);
  }

  dest.append(s->path_name_url.c_str());

  string& sub = s->args.get_sub_resource();
  if (sub.size() > 0) {
    dest.append("?");
    dest.append(sub);
  }
}

static void get_auth_header(struct req_state *s, string& dest, bool qsr)
{
  dest = "";
  if (s->method)
    dest = s->method;
  dest.append("\n");
  
  const char *md5 = FCGX_GetParam("HTTP_CONTENT_MD5", s->fcgx->envp);
  if (md5)
    dest.append(md5);
  dest.append("\n");

  const char *type = FCGX_GetParam("CONTENT_TYPE", s->fcgx->envp);
  if (type)
    dest.append(type);
  dest.append("\n");

  string date;
  if (qsr) {
    date = s->args.get("Expires");
  } else {
    const char *str = FCGX_GetParam("HTTP_DATE", s->fcgx->envp);
    if (str)
      date = str;
  }

  if (date.size())
      dest.append(date);
  dest.append("\n");

  string canon_amz_hdr;
  get_canon_amz_hdr(s, canon_amz_hdr);
  dest.append(canon_amz_hdr);

  string canon_resource;
  get_canon_resource(s, canon_resource);
  dest.append(canon_resource);
}

static void buf_to_hex(const unsigned char *buf, int len, char *str)
{
  int i;
  str[0] = '\0';
  for (i = 0; i < len; i++) {
    sprintf(&str[i*2], "%02x", (int)buf[i]);
  }
}

static void calc_hmac_sha1(const char *key, int key_len,
                           const char *msg, int msg_len,
                           char *dest, int *len) /* dest should be large enough to hold result */
{
  char hex_str[128];
  unsigned char *result = HMAC(EVP_sha1(), key, key_len, (const unsigned char *)msg,
                               msg_len, (unsigned char *)dest, (unsigned int *)len);

  buf_to_hex(result, *len, hex_str);

  cerr << "hmac=" << hex_str << std::endl;
}

static void init_state(struct req_state *s, struct fcgx_state *fcgx)
{
  char *p;
  for (int i=0; (p = fcgx->envp[i]); ++i) {
    cerr << p << std::endl;
  }
  s->fcgx = fcgx;
  s->content_started = false;
  s->indent = 0;
  s->path_name = FCGX_GetParam("SCRIPT_NAME", fcgx->envp);
  s->path_name_url = FCGX_GetParam("REQUEST_URI", fcgx->envp);
  int pos = s->path_name_url.find('?');
  if (pos >= 0)
    s->path_name_url = s->path_name_url.substr(0, pos);
  s->method = FCGX_GetParam("REQUEST_METHOD", fcgx->envp);
  s->host = FCGX_GetParam("HTTP_HOST", fcgx->envp);
  s->query = FCGX_GetParam("QUERY_STRING", fcgx->envp);
  s->length = FCGX_GetParam("CONTENT_LENGTH", fcgx->envp);
  s->content_type = FCGX_GetParam("CONTENT_TYPE", fcgx->envp);
  s->err_exist = false;
  memset(&s->err, 0, sizeof(s->err));

  if (!s->method)
    s->op = OP_UNKNOWN;
  else if (strcmp(s->method, "GET") == 0)
    s->op = OP_GET;
  else if (strcmp(s->method, "PUT") == 0)
    s->op = OP_PUT;
  else if (strcmp(s->method, "DELETE") == 0)
    s->op = OP_DELETE;
  else if (strcmp(s->method, "HEAD") == 0)
    s->op = OP_HEAD;
  else
    s->op = OP_UNKNOWN;

  init_entities_from_header(s);
  cerr << "s->object=" << (s->object ? s->object : "<NULL>") << " s->bucket=" << (s->bucket ? s->bucket : "<NULL>") << std::endl;

  init_auth_info(s);

  if (s->acl) {
     delete s->acl;
     s->acl = new S3AccessControlPolicy;
  }
}

static void do_list_buckets(struct req_state *s)
{
  int r;
  S3UserBuckets buckets;

  r = s3_get_user_buckets(s->user.user_id, buckets);
  if (r < 0) {
    /* hmm.. something wrong here.. the user was authenticated, so it
       should exist, just try to recreate */
    cerr << "WARNING: failed on s3_get_user_buckets uid=" << s->user.user_id << std::endl;
    s3_put_user_buckets(s->user.user_id, buckets);
    r = 0;
  }

  dump_errno(s, r);
  end_header(s, "application/xml");
  dump_start_xml(s);

  list_all_buckets_start(s);
  dump_owner(s, s->user.user_id, s->user.display_name);

  map<string, S3ObjEnt>& m = buckets.get_buckets();
  map<string, S3ObjEnt>::iterator iter;

  open_section(s, "Buckets");
  for (iter = m.begin(); iter != m.end(); ++iter) {
    S3ObjEnt obj = iter->second;
    dump_bucket(s, obj);
  }
  close_section(s, "Buckets");
  list_all_buckets_end(s);

  return;
}

char hex_to_num(char c)
{
  static char table[256];
  static bool initialized = false;


  if (!initialized) {
    memset(table, -1, sizeof(table));
    int i;
    for (i = '0'; i<='9'; i++)
      table[i] = i - '0';
    for (i = 'A'; i<='F'; i++)
      table[i] = i - 'A' + 0xa;
    for (i = 'a'; i<='f'; i++)
      table[i] = i - 'a' + 0xa;
  }
  return table[(int)c];
}

bool url_decode(string& src_str, string& dest_str)
{
  const char *src = src_str.c_str();
  char dest[src_str.size()];
  int pos = 0;
  char c;

  while (*src) {
    if (*src != '%') {
      dest[pos++] = *src++;
    } else {
      src++;
      char c1 = hex_to_num(*src++);
      c = c1 << 4;
      if (c1 < 0)
        return false;
      c1 = hex_to_num(*src++);
      if (c1 < 0)
        return false;
      c |= c1;
      dest[pos++] = c;
    }
  }
  dest[pos] = 0;
  dest_str = dest;

  return true;
}

static bool verify_signature(struct req_state *s)
{
  const char *http_auth = FCGX_GetParam("HTTP_AUTHORIZATION", s->fcgx->envp);
  bool qsr = false;
  string auth_id;
  string auth_sign;

  if (!http_auth || !(*http_auth)) {
    auth_id = s->args.get("AWSAccessKeyId");
    if (auth_id.size()) {
      url_decode(s->args.get("Signature"), auth_sign);

      string date = s->args.get("Expires");
      time_t exp = atoll(date.c_str());
      time_t now;
      time(&now);
      if (now >= exp)
        return false;

      qsr = true;
    } else {
      /* anonymous access */
      s3_get_anon_user(s->user);
      return true;
    }
  } else {
    if (strncmp(http_auth, "AWS ", 4))
      return false;
    string auth_str(http_auth + 4);
    int pos = auth_str.find(':');
    if (pos < 0)
      return false;

    auth_id = auth_str.substr(0, pos);
    auth_sign = auth_str.substr(pos + 1);
  }

  /* first get the user info */
  if (s3_get_user_info(auth_id, s->user) < 0) {
    cerr << "error reading user info, uid=" << auth_id << " can't authenticate" << std::endl;
    return false;
  }

  /* now verify signature */
   
  string auth_hdr;
  get_auth_header(s, auth_hdr, qsr);
  cerr << "auth_hdr:" << std::endl << auth_hdr << std::endl;

  const char *key = s->user.secret_key.c_str();
  int key_len = strlen(key);

  char hmac_sha1[EVP_MAX_MD_SIZE];
  int len;
  calc_hmac_sha1(key, key_len, auth_hdr.c_str(), auth_hdr.size(), hmac_sha1, &len);

  char b64[64]; /* 64 is really enough */
  int ret = encode_base64(hmac_sha1, len, b64, sizeof(b64));
  if (ret < 0) {
    cerr << "encode_base64 failed" << std::endl;
    return false;
  }

  cerr << "b64=" << b64 << std::endl;
  cerr << "auth_sign=" << auth_sign << std::endl;
  cerr << "compare=" << auth_sign.compare(b64) << std::endl;
  return (auth_sign.compare(b64) == 0);
}

static bool __verify_permission(S3AccessControlPolicy *policy, string& uid, int perm)
{
   if (!policy)
     return false;

   int acl_perm = policy->get_perm(uid, perm);

   return (perm == acl_perm);
}

static bool verify_permission(struct req_state *s, int perm)
{
  return __verify_permission(s->acl, s->user.user_id, perm);
}

static void get_object(struct req_state *s, bool get_data)
{
  S3GetObj_REST op(s, get_data);

  op.execute();
}

static int rebuild_policy(S3AccessControlPolicy& src, S3AccessControlPolicy& dest)
{
  ACLOwner *owner = (ACLOwner *)src.find_first("Owner");
  if (!owner)
    return -EINVAL;

  S3UserInfo owner_info;
  if (s3_get_user_info(owner->get_id(), owner_info) < 0) {
    cerr << "owner info does not exist" << std::endl;
    return -EINVAL;
  }
  ACLOwner& new_owner = dest.get_owner();
  new_owner.set_id(owner->get_id());
  new_owner.set_name(owner_info.display_name);

  S3AccessControlList& src_acl = src.get_acl();
  S3AccessControlList& acl = dest.get_acl();

  XMLObjIter iter = src_acl.find("Grant");
  ACLGrant *src_grant = (ACLGrant *)iter.get_next();
  while (src_grant) {
    ACLGranteeType& type = src_grant->get_type();
    ACLGrant new_grant;
    bool grant_ok = false;
    string id;
    switch (type.get_type()) {
    case ACL_TYPE_EMAIL_USER:
      {
        string email = src_grant->get_id();
        cerr << "grant user email=" << email << std::endl;
        if (s3_get_uid_by_email(email, id) < 0) {
          cerr << "grant user email not found or other error" << std::endl;
          break;
        }
      }
    case ACL_TYPE_CANON_USER:
      {
        if (type.get_type() == ACL_TYPE_CANON_USER)
          id = src_grant->get_id();
    
        S3UserInfo grant_user;
        if (s3_get_user_info(id, grant_user) < 0) {
          cerr << "grant user does not exist:" << id << std::endl;
        } else {
          ACLPermission& perm = src_grant->get_permission();
          new_grant.set_canon(id, grant_user.display_name, perm.get_permissions());
          grant_ok = true;
          cerr << "new grant: " << new_grant.get_id() << ":" << grant_user.display_name << std::endl;
        }
      }
      break;
    case ACL_TYPE_GROUP:
      {
        string group = src_grant->get_id();
        if (group.compare(S3_URI_ALL_USERS) == 0 ||
            group.compare(S3_URI_AUTH_USERS) == 0) {
          new_grant = *src_grant;
          grant_ok = true;
          cerr << "new grant: " << new_grant.get_id() << std::endl;
        }
      }
    default:
      /* FIXME: implement email based grant */
      break;
    }
    if (grant_ok) {
      acl.add_grant(&new_grant);
    }
    src_grant = (ACLGrant *)iter.get_next();
  }

  return 0; 
}

static void do_write_acls(struct req_state *s)
{
  bufferlist bl;
  int r = 0;

  size_t cl = atoll(s->length);
  size_t actual = 0;
  char *data = NULL;
  S3AccessControlPolicy *policy;
  S3XMLParser parser;
  S3AccessControlPolicy new_policy;

  if (!verify_permission(s, S3_PERM_WRITE_ACP)) {
    abort_early(s, -EACCES);
    return;
  }

  if (!parser.init()) {
    r = -EINVAL;
    goto done;
  }

  if (!s->acl) {
     s->acl = new S3AccessControlPolicy;
     if (!s->acl) {
       r = -ENOMEM;
       goto done;
     }
  }

  if (cl) {
    data = (char *)malloc(cl + 1);
    if (!data) {
       r = -ENOMEM;
       goto done;
    }
    actual = FCGX_GetStr(data, cl, s->fcgx->in);
    data[actual] = '\0';
  }

  cerr << "read data=" << data << " actual=" << actual << std::endl;


  if (!parser.parse(data, actual, 1)) {
    r = -EACCES;
    goto done;
  }
  policy = (S3AccessControlPolicy *)parser.find_first("AccessControlPolicy");
  if (!policy) {
    r = -EINVAL;
    goto done;
  }
  policy->to_xml(cerr);
  cerr << std::endl;

  r = rebuild_policy(*policy, new_policy);
  if (r < 0)
    goto done;

  cerr << "new_policy: ";
  new_policy.to_xml(cerr);
  cerr << std::endl;

  /* FIXME: make some checks around checks and fix policy */

  new_policy.encode(bl);
  r = s3store->set_attr(s->bucket_str, s->object_str,
                       S3_ATTR_ACL, bl);

done:
  free(data);

  dump_errno(s, r);
  end_header(s, "application/xml");
  dump_start_xml(s);
  return;
}

static int __read_acls(S3AccessControlPolicy *policy, string& bucket, string& object)
{
  bufferlist bl;
  int ret = 0;

  if (bucket.size()) {
    ret = s3store->get_attr(bucket, object,
                       S3_ATTR_ACL, bl);

    if (ret >= 0) {
      bufferlist::iterator iter = bl.begin();
      policy->decode(iter);
      policy->to_xml(cerr);
    }
  }

  return ret;
}

static int read_acls(struct req_state *s, bool only_bucket = false)
{
  int ret = 0;
  string obj_str;

  if (!s->acl) {
     s->acl = new S3AccessControlPolicy;
     if (!s->acl)
       return -ENOMEM;
  }

  /* we're passed only_bucket = true when we specifically need the bucket's
     acls, that happens on write operations */
  if (!only_bucket)
    obj_str = s->object_str;

  ret = __read_acls(s->acl, s->bucket_str, obj_str);

  return ret;
}

static void get_acls(struct req_state *s)
{
  int ret = read_acls(s);

  if (ret < 0) {
    /* FIXME */
  }

  stringstream ss;
  s->acl->to_xml(ss);
  string str = ss.str(); 
  end_header(s, "application/xml");
  dump_start_xml(s);
  FCGX_PutStr(str.c_str(), str.size(), s->fcgx->out); 
}

static bool is_acl_op(struct req_state *s)
{
  return s->args.exists("acl");
}

static void do_retrieve_objects(struct req_state *s, bool get_data)
{
  if (is_acl_op(s)) {
    if (!verify_permission(s, S3_PERM_READ_ACP)) {
      abort_early(s, -EACCES);
      return;
    }

    get_acls(s);
    return;
  }

  if (!verify_permission(s, S3_PERM_READ)) {
    abort_early(s, -EACCES);
    return;
  }

  if (s->object) {
    get_object(s, get_data);
    return;
  } else if (!s->bucket) {
    return;
  }

  S3ListBucket_REST op(s);
  op.execute();
}

static void get_canned_acl_request(struct req_state *s, string& canned_acl)
{
  const char *param = FCGX_GetParam("HTTP_X_AMZ_ACL", s->fcgx->envp);

  if (param)
    canned_acl = param;
  else
    canned_acl.clear();
}

static void do_create_bucket(struct req_state *s)
{
  S3AccessControlPolicy policy;
  string canned_acl;
  int r;
  map<nstring, bufferlist> attrs;
  bufferlist aclbl;

  get_canned_acl_request(s, canned_acl);

  bool ret = policy.create_canned(s->user.user_id, s->user.display_name, canned_acl);

  if (!ret) {
    r = -EINVAL;
    goto done;
  }
  policy.encode(aclbl);

  attrs[S3_ATTR_ACL] = aclbl;

  r = s3store->create_bucket(s->user.user_id, s->bucket_str, attrs);

  if (r == 0) {
    S3UserBuckets buckets;

    int ret = s3_get_user_buckets(s->user.user_id, buckets);
    S3ObjEnt new_bucket;

    switch (ret) {
    case 0:
    case -ENOENT:
    case -ENODATA:
      new_bucket.name = s->bucket_str;
      new_bucket.size = 0;
      time(&new_bucket.mtime);
      buckets.add(new_bucket);
      r = s3_put_user_buckets(s->user.user_id, buckets);
      break;
    default:
      cerr << "s3_get_user_buckets returned " << ret << std::endl;
      break;
    }
  }

done:
  dump_errno(s, r);
  end_header(s);
}

static bool parse_copy_source(const char *src, string& bucket, string& object)
{
  string url_src(src);
  string dec_src;

  url_decode(url_src, dec_src);
  src = dec_src.c_str();

  cerr << "decoded src=" << src << std::endl;

  if (*src == '/') ++src;

  string str(src);

  int pos = str.find("/");
  if (pos <= 0)
    return false;

  bucket = str.substr(0, pos);
  object = str.substr(pos + 1);

  if (object.size() == 0)
    return false;

  return true;
}

static void copy_object(struct req_state *s, const char *copy_source)
{
  int r = -EINVAL;
  char *data = NULL;
  struct s3_err err;
  S3AccessControlPolicy dest_policy;
  bool ret;
  bufferlist aclbl;
  map<nstring, bufferlist> attrs;
  bufferlist bl;
  S3AccessControlPolicy src_policy;
  string src_bucket, src_object, empty_str;
  const char *if_mod = FCGX_GetParam("HTTP_X_AMZ_COPY_IF_MODIFIED_SINCE", s->fcgx->envp);
  const char *if_unmod = FCGX_GetParam("HTTP_X_AMZ_COPY_IF_UNMODIFIED_SINCE", s->fcgx->envp);
  const char *if_match = FCGX_GetParam("HTTP_X_AMZ_COPY_IF_MATCH", s->fcgx->envp);
  const char *if_nomatch = FCGX_GetParam("HTTP_X_AMZ_COPY_IF_NONE_MATCH", s->fcgx->envp);
  time_t mod_time;
  time_t unmod_time;
  time_t *mod_ptr = NULL;
  time_t *unmod_ptr = NULL;
  time_t mtime;

  if (!verify_permission(s, S3_PERM_WRITE)) {
    abort_early(s, -EACCES);
    return;
  }

  string canned_acl;
  get_canned_acl_request(s, canned_acl);

  ret = dest_policy.create_canned(s->user.user_id, s->user.display_name, canned_acl);
  if (!ret) {
     err.code = "InvalidArgument";
     r = -EINVAL;
     goto done;
  }

  copy_source = FCGX_GetParam("HTTP_X_AMZ_COPY_SOURCE", s->fcgx->envp);
  ret = parse_copy_source(copy_source, src_bucket, src_object);
  if (!ret) {
     err.code = "InvalidArgument";
     r = -EINVAL;
     goto done;
  }
  /* just checking the bucket's permission */
  r = __read_acls(&src_policy, src_bucket, empty_str);
  if (r < 0) {
    goto done;
  }
  if (!__verify_permission(&src_policy, s->user.user_id, S3_PERM_READ)) {
    abort_early(s, -EACCES);
    return;
  }

  dest_policy.encode(aclbl);

  if (if_mod) {
    if (parse_time(if_mod, &mod_time) < 0)
      goto done;
    mod_ptr = &mod_time;
  }

  if (if_unmod) {
    if (parse_time(if_unmod, &unmod_time) < 0)
      goto done;
    unmod_ptr = &unmod_time;
  }

  attrs[S3_ATTR_ACL] = aclbl;
  get_request_metadata(s, attrs);

  r = s3store->copy_obj(s->user.user_id,
                        s->bucket_str, s->object_str,
                        src_bucket, src_object,
                        &mtime,
                        mod_ptr,
                        unmod_ptr,
                        if_match,
                        if_nomatch,
                        attrs, &err);

done:
  free(data);
  dump_errno(s, r, &err);

  end_header(s);
  if (r == 0) {
    open_section(s, "CopyObjectResult");
    dump_time(s, "LastModified", &mtime);
    map<nstring, bufferlist>::iterator iter = attrs.find(S3_ATTR_ETAG);
    if (iter != attrs.end()) {
      bufferlist& bl = iter->second;
      if (bl.length()) {
        char *etag = bl.c_str();
        dump_value(s, "ETag", etag);
      }
    }
    close_section(s, "CopyObjectResult");
  }
}

static void do_create_object(struct req_state *s)
{
  int r = -EINVAL;
  char *data = NULL;
  struct s3_err err;
  if (!s->object) {
    goto done;
  } else {
    const char *copy_source = FCGX_GetParam("HTTP_X_AMZ_COPY_SOURCE", s->fcgx->envp);
    if (copy_source) {
      copy_object(s, copy_source);
      return;
    }
    S3AccessControlPolicy policy;

    if (!verify_permission(s, S3_PERM_WRITE)) {
      abort_early(s, -EACCES);
      return;
    }

    string canned_acl;
    get_canned_acl_request(s, canned_acl);

    bool ret = policy.create_canned(s->user.user_id, s->user.display_name, canned_acl);
    if (!ret) {
       err.code = "InvalidArgument";
       r = -EINVAL;
       goto done;
    }
    size_t cl = atoll(s->length);
    size_t actual = 0;
    if (cl) {
      data = (char *)malloc(cl);
      if (!data) {
         r = -ENOMEM;
         goto done;
      }
      actual = FCGX_GetStr(data, cl, s->fcgx->in);
    }

    char *supplied_md5_b64 = FCGX_GetParam("HTTP_CONTENT_MD5", s->fcgx->envp);
    char supplied_md5_bin[MD5_DIGEST_LENGTH + 1];
    char supplied_md5[MD5_DIGEST_LENGTH * 2 + 1];
    char calc_md5[MD5_DIGEST_LENGTH * 2 + 1];
    MD5_CTX c;
    unsigned char m[MD5_DIGEST_LENGTH];

    if (supplied_md5_b64) {
      cerr << "supplied_md5_b64=" << supplied_md5_b64 << std::endl;
      int ret = decode_base64(supplied_md5_b64, strlen(supplied_md5_b64),
                                 supplied_md5_bin, sizeof(supplied_md5_bin));
      cerr << "decode_base64 ret=" << ret << std::endl;
      if (ret != MD5_DIGEST_LENGTH) {
        err.code = "InvalidDigest";
        r = -EINVAL;
        goto done;
      }

      buf_to_hex((const unsigned char *)supplied_md5_bin, MD5_DIGEST_LENGTH, supplied_md5);
      cerr << "supplied_md5=" << supplied_md5 << std::endl;
    }

    MD5_Init(&c);
    MD5_Update(&c, data, (unsigned long)actual);
    MD5_Final(m, &c);

    buf_to_hex(m, MD5_DIGEST_LENGTH, calc_md5);

    if (supplied_md5_b64 && strcmp(calc_md5, supplied_md5)) {
       err.code = "BadDigest";
       r = -EINVAL;
       goto done;
    }
    bufferlist aclbl;
    policy.encode(aclbl);

    string md5_str(calc_md5);
    map<nstring, bufferlist> attrs;
    bufferlist bl;
    bl.append(md5_str.c_str(), md5_str.size() + 1);
    attrs[S3_ATTR_ETAG] = bl;
    attrs[S3_ATTR_ACL] = aclbl;

    if (s->content_type) {
      bl.clear();
      bl.append(s->content_type, strlen(s->content_type) + 1);
      attrs[S3_ATTR_CONTENT_TYPE] = bl;
    }

    get_request_metadata(s, attrs);

    r = s3store->put_obj(s->user.user_id, s->bucket_str, s->object_str, data, actual, NULL, attrs);
  }
done:
  free(data);
  dump_errno(s, r, &err);

  end_header(s);
}

static void do_delete_bucket(struct req_state *s)
{
  int r = -EINVAL;

  if (!verify_permission(s, S3_PERM_WRITE)) {
    abort_early(s, -EACCES);
    return;
  }

  if (s->bucket) {
    r = s3store->delete_bucket(s->user.user_id, s->bucket_str);

    if (r == 0) {
      S3UserBuckets buckets;

      int ret = s3_get_user_buckets(s->user.user_id, buckets);

      if (ret == 0 || ret == -ENOENT) {
        buckets.remove(s->bucket_str);
        r = s3_put_user_buckets(s->user.user_id, buckets);
      }
    }
  }

  dump_errno(s, r);
  end_header(s);
}

static void do_delete_object(struct req_state *s)
{
  int r = -EINVAL;
  if (s->object) {
    r = s3store->delete_obj(s->user.user_id, s->bucket_str, s->object_str);
  }

  dump_errno(s, r);
  end_header(s);
}

static void do_retrieve(struct req_state *s, bool get_data)
{
  if (s->bucket)
    do_retrieve_objects(s, get_data);
  else
    do_list_buckets(s);
}

static void do_create(struct req_state *s)
{
  if (is_acl_op(s))
    do_write_acls(s);
  else if (s->object)
    do_create_object(s);
  else if (s->bucket)
    do_create_bucket(s);
  else
    return;
}

static void do_delete(struct req_state *s)
{
  if (s->object)
    do_delete_object(s);
  else if (s->bucket)
    do_delete_bucket(s);
  else
    return;
}

static sighandler_t sighandler;

static void godown(int signum)
{
  BackTrace bt(0);
  bt.print(cerr);

  signal(SIGSEGV, sighandler);
}

int read_permissions(struct req_state *s)
{
  bool only_bucket;

  switch (s->op) {
  case OP_HEAD:
  case OP_GET:
    only_bucket = false;
    break;
  case OP_PUT:
    /* is it a 'create bucket' request? */
    if (s->object_str.size() == 0)
      return 0;
    if (is_acl_op(s)) {
      only_bucket = false;
      break;
    }
  case OP_DELETE:
    only_bucket = true;
    break;
  default:
    return -EINVAL;
  }

  int ret = read_acls(s, only_bucket);

  if (ret < 0)
    cerr << "read_permissions on " << s->bucket_str << ":" <<s->object_str << " only_bucket=" << only_bucket << " ret=" << ret << std::endl;

  return ret;
}

int main(int argc, char *argv[])
{
  struct req_state s;
  struct fcgx_state fcgx;

  if (!S3Access::init_storage_provider("rados", argc, argv)) {
    cerr << "couldn't init storage provider" << std::endl;
  }

  sighandler = signal(SIGSEGV, godown);

  while (FCGX_Accept(&fcgx.in, &fcgx.out, &fcgx.err, &fcgx.envp) >= 0) 
  {
    init_state(&s, &fcgx);

    int ret = read_acls(&s);
    if (ret < 0) {
      switch (ret) {
      case -ENOENT:
        break;
      default:
        cerr << "could not read acls" << " ret=" << ret << std::endl;
        abort_early(&s, -EPERM);
        continue;
      }
    }
    ret = verify_signature(&s);
    if (!ret) {
      cerr << "signature DOESN'T match" << std::endl;
      abort_early(&s, -EPERM);
      continue;
    }

    ret = read_permissions(&s);
    if (ret < 0) {
      abort_early(&s, ret);
      continue;
    }

    switch (s.op) {
    case OP_GET:
      do_retrieve(&s, true);
      break;
    case OP_PUT:
      do_create(&s);
      break;
    case OP_DELETE:
      do_delete(&s);
      break;
    case OP_HEAD:
      do_retrieve(&s, false);
      break;
    default:
      abort_early(&s, -EACCES);
      break;
    }
  }
  return 0;
}

