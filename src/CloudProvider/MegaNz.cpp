/*****************************************************************************
 * MegaNz.cpp : Mega implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef WITH_MEGA

#include "MegaNz.h"

#include "IAuth.h"
#include "Request/DownloadFileRequest.h"
#include "Request/Request.h"
#include "Utility/FileServer.h"
#include "Utility/Item.h"
#include "Utility/Utility.h"

#include <json/json.h>
#include <array>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <queue>

#undef DELETE

using namespace mega;
using namespace std::placeholders;

const int HASH_BUFFER_SIZE = 128;

namespace cloudstorage {

namespace {

enum class Type {
  FETCH_NODES,
  MOVE,
  UPLOAD,
  RENAME,
  DELETE,
  READ,
  MKDIR,
  GENERAL_DATA,
  LOGIN
};

std::string error_description(error e) {
  if (e <= 0) {
    switch (e) {
      case API_OK:
        return "No error";
      case API_EINTERNAL:
        return "Internal error";
      case API_EARGS:
        return "Invalid argument";
      case API_EAGAIN:
        return "Request failed, retrying";
      case API_ERATELIMIT:
        return "Rate limit exceeded";
      case API_EFAILED:
        return "Failed permanently";
      case API_ETOOMANY:
        return "Too many concurrent connections or transfers";
      case API_ERANGE:
        return "Out of range";
      case API_EEXPIRED:
        return "Expired";
      case API_ENOENT:
        return "Not found";
      case API_ECIRCULAR:
        return "Circular linkage detected";
      case API_EACCESS:
        return "Access denied";
      case API_EEXIST:
        return "Already exists";
      case API_EINCOMPLETE:
        return "Incomplete";
      case API_EKEY:
        return "Invalid key/Decryption error";
      case API_ESID:
        return "Bad session ID";
      case API_EBLOCKED:
        return "Blocked";
      case API_EOVERQUOTA:
        return "Over quota";
      case API_ETEMPUNAVAIL:
        return "Temporarily not available";
      case API_ETOOMANYCONNECTIONS:
        return "Connection overflow";
      case API_EWRITE:
        return "Write error";
      case API_EREAD:
        return "Read error";
      case API_EAPPKEY:
        return "Invalid application key";
      case API_ESSL:
        return "SSL verification failed";
      case API_EGOINGOVERQUOTA:
        return "Not enough quota";
      default:
        return "Unknown error";
    }
  }
  return "HTTP Error";
}

template <class Result>
class Listener : public IRequest<EitherError<Result>> {
 public:
  using Callback = std::function<void(EitherError<Result>)>;

  static constexpr int IN_PROGRESS = -1;
  static constexpr int FAILURE = 0;
  static constexpr int SUCCESS = 1;
  static constexpr int CANCELLED = 2;
  static constexpr int PAUSED = 3;

  Listener(Callback cb)
      : status_(IN_PROGRESS),
        error_({IHttpRequest::Unknown, ""}),
        callback_(cb) {}

  ~Listener() override { cancel(); }

  void cancel() override {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (status_ != IN_PROGRESS) return;
    status_ = CANCELLED;
    error_ = {IHttpRequest::Aborted, util::Error::ABORTED};
    auto callback = util::exchange(callback_, nullptr);
    download_callback_ = nullptr;
    upload_callback_ = nullptr;
    lock.unlock();
    if (callback) callback(error_);
    finish();
  }

  EitherError<Result> result() override {
    finish();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (status_ != SUCCESS)
      return error_;
    else
      return result_;
  }

  void finish() override {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return status_ != IN_PROGRESS; });
  }

  void pause() override {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (status_ != CANCELLED) status_ = PAUSED;
  }

  void resume() override {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (status_ == PAUSED) status_ = IN_PROGRESS;
  }

  int status() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return status_;
  }

  void done(EitherError<Result> e) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    status_ = SUCCESS;
    result_ = e;
    auto callback = util::exchange(callback_, nullptr);
    download_callback_ = nullptr;
    upload_callback_ = nullptr;
    lock.unlock();
    if (callback) callback(e);
    condition_.notify_all();
  }

  bool receivedData(const char* data, uint32_t length) {
    if (download_callback_) {
      download_callback_->receivedData(data, length);
      download_callback_->progress(total_bytes_, received_bytes_);
    }
    return status_ == IN_PROGRESS;
  }

  std::unique_lock<std::recursive_mutex> lock() {
    return std::unique_lock<std::recursive_mutex>(mutex_);
  }

  IDownloadFileCallback* download_callback_ = nullptr;
  IUploadFileCallback* upload_callback_ = nullptr;
  uint64_t received_bytes_ = 0;
  uint64_t total_bytes_ = 0;

 protected:
  int status_;
  std::recursive_mutex mutex_;
  std::condition_variable_any condition_;
  Error error_;
  Callback callback_;
  EitherError<Result> result_;
};

struct App : public MegaApp {
  App(MegaNz* mega) : mega_(mega) {}

  void notify_retry(dstime, retryreason_t) override { client->abortbackoff(); }

  void transfer_failed(Transfer*, error, dstime) override {
    client->abortbackoff();
  }

  dstime pread_failure(error e, int retry, void* d, dstime) override {
    auto it = callback_.find(reinterpret_cast<uintptr_t>(d));
    if (retry >= 4 && it != callback_.end()) {
      auto request = static_cast<Listener<error>*>(it->second.second.get());
      request->done(Error{e, error_description(e)});
      callback_.erase(it);
    }
    return 0;
  }

  bool pread_data(uint8_t* data, m_off_t length, m_off_t, m_off_t, m_off_t,
                  void* d) override {
    auto it = callback_.find(reinterpret_cast<uintptr_t>(d));
    if (it != callback_.end()) {
      auto request = static_cast<Listener<error>*>(it->second.second.get());
      auto lock = request->lock();
      request->received_bytes_ += length;
      auto result =
          request->receivedData(reinterpret_cast<const char*>(data), length);
      if (request->received_bytes_ == request->total_bytes_) {
        request->done(std::make_shared<error>(API_OK));
        callback_.erase(it);
      } else if (result == false) {
        callback_.erase(it);
      }
      return result;
    } else {
      return false;
    }
  }

  void account_details(AccountDetails* details, bool, bool, bool, bool, bool,
                       bool) override {
    GeneralData data;
    data.space_total_ = details->storage_max;
    data.space_used_ = details->storage_used;
    call(API_OK, data);
  }

  void login_result(error e) override { call(e, e); }

  void fetchnodes_result(error e) override { call(e, e); }

  void nodes_updated(Node** node, int) override {
    auto it = callback_.find(this->client->restag);
    if (it != callback_.end()) {
      if (it->second.first == Type::MKDIR || it->second.first == Type::MOVE)
        call(API_OK, node[0]->nodehandle);
    }
  }

  void putnodes_result(error e, targettype_t, NewNode* nodes) override {
    auto it = callback_.find(this->client->restag);
    if (it != callback_.end() && it->second.first == Type::UPLOAD)
      delete[] nodes;
    if (e == API_OK) {
      call(e, client->nodenotify.back()->nodehandle);
    } else {
      call(e, 0ULL);
    }
  }

  void unlink_result(handle, error e) override { call(e, e); }

  void rename_result(handle h, error e) override { call(e, h); }

  void setattr_result(handle, error e) override { call(e, e); }

  void exec() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (exec_pending_ || removed_) return;
    exec_pending_ = true;
    client->exec();
    exec_pending_ = false;
  }

  template <class T>
  void call(error e, const T& arg) {
    auto it = callback_.find(this->client->restag);
    if (it != callback_.end()) {
      auto r = static_cast<Listener<T>*>(it->second.second.get());
      if (e == API_OK)
        r->done(EitherError<T>(std::make_shared<T>(arg)));
      else
        r->done(Error{e, error_description(e)});
      callback_.erase(it);
    }
  }

  std::unique_lock<std::recursive_mutex> lock() {
    return std::unique_lock<std::recursive_mutex>(mutex_);
  }

  std::recursive_mutex mutex_;
  MegaNz* mega_;
  std::unordered_map<int, std::pair<Type, std::shared_ptr<IGenericRequest>>>
      callback_;
  bool exec_pending_ = false;
  bool removed_ = false;
};

struct CloudHttp : public HttpIO {
  class HttpCallback : public IHttpRequest::ICallback {
   public:
    HttpCallback(App* app, std::function<void(const char*, uint32_t)> read,
                 std::shared_ptr<std::atomic_bool> abort)
        : app_(app), stream_(read), abort_(abort), progress_() {}

    ~HttpCallback() override {}

    bool isSuccess(int code,
                   const IHttpRequest::HeaderParameters&) const override {
      return IHttpRequest::isSuccess(code);
    }

    bool abort() override { return *abort_; }

    bool pause() override { return false; }

    void progressDownload(uint64_t, uint64_t now) override {
      progress_ = now;
      app_->exec();
    }

    void progressUpload(uint64_t, uint64_t now) override {
      progress_ = now;
      app_->exec();
    }

    App* app_;
    DownloadStreamWrapper stream_;
    std::shared_ptr<std::atomic_bool> abort_;
    std::atomic_uint64_t progress_;
  };

  void post(struct HttpReq* r, const char* data, unsigned length) override {
    auto lock = app_->lock();
    if (http_ == nullptr) return;
    auto abort_mark = std::make_shared<std::atomic_bool>(false);
    auto request = http_->create(r->posturl, "POST");
    auto callback = std::make_shared<HttpCallback>(
        app_,
        [=](const char* d, uint32_t cnt) {
          auto lock = app_->lock();
          if (!*abort_mark)
            read_update_.push_back(std::make_pair(r, std::string(d, cnt)));
          app_->exec();
        },
        abort_mark);
    auto input = std::make_shared<std::stringstream>();
    auto output = std::make_shared<std::ostream>(&callback->stream_);
    if (!data) {
      *input << std::string(r->out->data(), r->out->size());
    } else {
      *input << std::string(data, length);
    }
    r->status = REQ_INFLIGHT;
    r->httpiohandle = new std::shared_ptr<HttpCallback>(callback);
    pending_requests_++;
    request->send(
        [=](EitherError<IHttpRequest::Response> e) {
          auto lock = app_->lock();
          pending_requests_--;
          if (!http_ && pending_requests_ == 0) {
            no_requests_.set_value();
          } else if (!*abort_mark) {
            queue_.push_back({r, e});
            app_->exec();
          }
        },
        input, output, output, callback);
  }

  void cancel(HttpReq* h) override {
    auto lock = app_->lock();
    h->httpstatus = 0;
    h->httpio = nullptr;
    h->status = REQ_FAILURE;
    if (h->httpiohandle) {
      auto r = static_cast<std::shared_ptr<HttpCallback>*>(h->httpiohandle);
      *((*r)->abort_) = true;
      delete r;
      h->httpiohandle = nullptr;
    }
    for (auto& d : read_update_)
      if (d.first == h) d.first = nullptr;
    for (auto& d : queue_)
      if (d.first == h) d.first = nullptr;
  }

  m_off_t postpos(void* h) override {
    return (*static_cast<std::shared_ptr<HttpCallback>*>(h))->progress_;
  }

  bool doio() override {
    auto lock = app_->lock();
    bool result = queue_.size() > 0;
    while (!read_update_.empty()) {
      auto r = read_update_.front();
      read_update_.pop_front();
      if (r.first) r.first->put((void*)r.second.c_str(), r.second.size());
    }
    while (!queue_.empty()) {
      auto r = queue_.back();
      queue_.pop_back();
      if (!r.first) continue;
      r.first->httpio = nullptr;
      delete static_cast<std::shared_ptr<HttpCallback>*>(r.first->httpiohandle);
      r.first->httpiohandle = nullptr;
      result = true;
      if (r.second.left()) {
        r.first->httpstatus = r.second.left()->code_;
        r.first->status = REQ_FAILURE;
      } else {
        auto d = r.second.right();
        auto req = r.first;
        req->httpstatus = d->http_code_;
        auto content_length_it = d->headers_.find("content-length");
        if (IHttpRequest::isSuccess(d->http_code_) &&
            content_length_it != d->headers_.end()) {
          req->contentlength = std::stol(content_length_it->second);
          req->status = REQ_SUCCESS;
          success = true;
        } else {
          req->status = REQ_FAILURE;
        }
      }
    }
    return result;
  }

  void addevents(Waiter*, int) override {}

  void setuseragent(string*) override {}

  CloudHttp(IHttp* http, App* app) : http_(http), app_(app) {}

  std::vector<std::pair<HttpReq*, EitherError<IHttpRequest::Response>>> queue_;
  std::deque<std::pair<HttpReq*, std::string>> read_update_;
  IHttp* http_;
  App* app_;
  uint32_t pending_requests_ = 0;
  std::promise<void> no_requests_;
};

class FileUpload : public File {
 public:
  void progress() override {
    if (listener_ && listener_->upload_callback_)
      listener_->upload_callback_->progress(size_,
                                            transfer->slot->progressreported);
  }

  void completed(Transfer* t, LocalNode* n) override {
    File::completed(t, n);
    delete this;
  }

  void terminated() override {
    listener_->done(Error{API_EFAILED, error_description(API_EFAILED)});
    delete this;
  }

  Listener<handle>* listener_ = nullptr;
  uint64_t size_ = 0;
};  // namespace

class CloudFileSystemAccess : public FileSystemAccess {
 public:
  class CloudFileAccess : public FileAccess {
   public:
    CloudFileAccess(CloudFileSystemAccess* fs)
        : FileAccess(nullptr), fs_(fs), callback_() {}

    bool asyncavailable() override { return false; }
    void updatelocalname(string* d) override { fopen(d, true, false); }
    bool fopen(string* str, bool, bool) override {
      localname = *str;
      type = FILENODE;
      retry = false;
      auto it = fs_->callback_.find(std::stoull(localname));
      if (it == fs_->callback_.end()) return false;
      callback_ = it->second;
      sysstat(&mtime, &size);
      return true;
    }
    bool fwrite(const uint8_t*, unsigned, m_off_t) override { return false; }
    bool sysread(uint8_t* data, unsigned length, m_off_t offset) override {
      retry = false;
      return callback_->putData((char*)data, length, offset) == length;
    }
    bool sysstat(m_time_t* time, m_off_t* size) override {
      *time = 0;
      *size = callback_->size();
      return true;
    }
    bool sysopen(bool) override { return fopen(&localname, true, false); }
    void sysclose() override {}

   private:
    CloudFileSystemAccess* fs_;
    IUploadFileCallback* callback_;
  };

  void tmpnamelocal(string*) const override {}
  bool getsname(string*, string*) const override { return false; }
  bool renamelocal(string*, string*, bool) override { return false; }
  bool copylocal(string*, string*, m_time_t) override { return false; }
  bool unlinklocal(string*) override { return false; }
  bool rmdirlocal(string*) override { return false; }
  bool mkdirlocal(string*, bool) override { return false; }
  bool setmtimelocal(string*, m_time_t) override { return false; }
  bool chdirlocal(string*) const override { return false; }
  size_t lastpartlocal(string*) const override { return 0; }
  bool getextension(string*, char*, int) const override { return false; }
  bool issyncsupported(string*, bool*) override { return false; }
  bool expanselocalpath(string*, string*) override { return false; }
  void addevents(Waiter*, int) override {}
  void local2path(string*, string*) const override {}
  void path2local(string*, string*) const override {}
  DirAccess* newdiraccess() override { return nullptr; }
  FileAccess* newfileaccess() override { return new CloudFileAccess(this); }

  std::unordered_map<uint32_t, IUploadFileCallback*> callback_;
  uint32_t tag_ = 0;
};

}  // namespace

class CloudMegaClient {
 public:
  CloudMegaClient(MegaNz* mega, const char* api_key)
      : app_(mega),
        http_(util::make_unique<CloudHttp>(mega->http(), &app_)),
        fs_(util::make_unique<CloudFileSystemAccess>()),
        client_(util::make_unique<MegaClient>(&app_, nullptr, http_.get(),
                                              fs_.get(), nullptr, nullptr,
                                              api_key, "libcloudstorage")) {}

  ~CloudMegaClient() {
    auto lock = this->lock();
    client_ = nullptr;
    app_.removed_ = true;
    http_->http_ = nullptr;
    if (http_->pending_requests_ > 0) {
      lock.unlock();
      http_->no_requests_.get_future().get();
      lock.lock();
    }
    fs_ = nullptr;
    http_ = nullptr;
  }

  MegaClient* client() { return client_.get(); }

  int register_callback(Type type, std::shared_ptr<IGenericRequest> request) {
    auto tag = client_->nextreqtag();
    app_.callback_[tag] = {type, request};
    return tag;
  }

  int register_file(IUploadFileCallback* callback) {
    auto tag = fs_->tag_++;
    fs_->callback_[tag] = callback;
    return tag;
  }

  void remove_file(int tag) { fs_->callback_.erase(fs_->callback_.find(tag)); }

  void exec() { app_.exec(); }

  std::unique_lock<std::recursive_mutex> lock() { return app_.lock(); }

 private:
  App app_;
  std::unique_ptr<CloudHttp> http_;
  std::unique_ptr<CloudFileSystemAccess> fs_;
  std::unique_ptr<MegaClient> client_;
};

MegaNz::MegaNz()
    : CloudProvider(util::make_unique<Auth>()), mega_(), authorized_() {}

MegaNz::~MegaNz() {}

Node* MegaNz::node(const std::string& id) const {
  if (id == rootDirectory()->id())
    return mega_->client()->nodebyhandle(mega_->client()->rootnodes[0]);
  else
    return mega_->client()->nodebyhandle(std::stoull(id));
}

void MegaNz::initialize(InitData&& data) {
  CloudProvider::initialize(std::move(data));
  auto lock = auth_lock();
  setWithHint(data.hints_, "client_id", [this](std::string v) {
    mega_ = util::make_unique<CloudMegaClient>(this, v.c_str());
  });
  if (!mega_) mega_ = util::make_unique<CloudMegaClient>(this, "ZVhB0Czb");
}

std::string MegaNz::name() const { return "mega"; }

std::string MegaNz::endpoint() const { return file_url(); }

void MegaNz::destroy() {
  CloudProvider::destroy();
  mega_ = nullptr;
}

ICloudProvider::ExchangeCodeRequest::Pointer MegaNz::exchangeCodeAsync(
    const std::string& code, ExchangeCodeCallback callback) {
  return std::make_shared<Request<EitherError<Token>>>(
             shared_from_this(), callback,
             [=](Request<EitherError<Token>>::Pointer r) {
               auto token = authorizationCodeToToken(code);
               auto ret = token->token_.empty()
                              ? EitherError<Token>(Error{
                                    IHttpRequest::Failure,
                                    util::Error::INVALID_AUTHORIZATION_CODE})
                              : EitherError<Token>({token->token_, ""});
               r->done(ret);
             })
      ->run();
}

AuthorizeRequest::Pointer MegaNz::authorizeAsync() {
  return std::make_shared<AuthorizeRequest>(
      shared_from_this(), [=](AuthorizeRequest::Pointer r,
                              AuthorizeRequest::AuthorizeCompleted complete) {
        auto fetch = [=]() {
          r->make_subrequest<MegaNz>(
              &MegaNz::make_request<error>, Type::FETCH_NODES,
              [=](Listener<error>*, int) {
                auto lock = mega_->lock();
                mega_->client()->fetchnodes();
                mega_->exec();
              },
              [=](EitherError<error> e) {
                if (e.left()) return complete(e.left());
                if (*e.right() != 0)
                  complete(Error{*e.right(), error_description(*e.right())});
                else {
                  authorized_ = true;
                  complete(nullptr);
                }
              });
        };
        login(r, [=](EitherError<void> e) {
          if (!e.left()) return fetch();
          if (auth_callback()->userConsentRequired(*this) ==
              ICloudProvider::IAuthCallback::Status::WaitForAuthorizationCode) {
            auto code = [=](EitherError<std::string> e) {
              if (e.left()) return complete(e.left());
              {
                auto lock = auth_lock();
                auth()->set_access_token(authorizationCodeToToken(*e.right()));
              }
              login(r, [=](EitherError<void> e) {
                if (e.left())
                  complete(e.left());
                else
                  fetch();
              });
            };
            r->set_server(
                r->provider()->auth()->requestAuthorizationCode(code));
          } else {
            complete(Error{IHttpRequest::Unauthorized,
                           util::Error::INVALID_CREDENTIALS});
          }
        });
      });
}

ICloudProvider::GetItemDataRequest::Pointer MegaNz::getItemDataAsync(
    const std::string& id, GetItemDataCallback callback) {
  return std::make_shared<Request<EitherError<IItem>>>(
             shared_from_this(), callback,
             [=](Request<EitherError<IItem>>::Pointer r) {
               ensureAuthorized<EitherError<IItem>>(r, [=] {
                 auto lock = mega_->lock();
                 auto node = this->node(id);
                 if (!node)
                   return r->done(Error{IHttpRequest::NotFound,
                                        util::Error::NODE_NOT_FOUND});
                 return r->done(toItem(node));
               });
             })
      ->run();
}

ICloudProvider::DownloadFileRequest::Pointer MegaNz::downloadFileAsync(
    IItem::Pointer item, IDownloadFileCallback::Pointer callback, Range range) {
  return std::make_shared<Request<EitherError<void>>>(
             shared_from_this(),
             [=](EitherError<void> e) { callback->done(e); },
             downloadResolver(item, callback.get(), range))
      ->run();
}

ICloudProvider::UploadFileRequest::Pointer MegaNz::uploadFileAsync(
    IItem::Pointer item, const std::string& filename,
    IUploadFileCallback::Pointer cb) {
  auto callback = cb.get();
  auto resolver = [=](Request<EitherError<IItem>>::Pointer r) {
    ensureAuthorized<EitherError<IItem>>(r, [=] {
      auto lock = mega_->lock();
      auto node = this->node(item->id());
      if (!node)
        return r->done(
            Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
      auto tag = std::make_shared<uint32_t>(0);
      r->make_subrequest<MegaNz>(
          &MegaNz::make_request<handle>, Type::UPLOAD,
          [=](Listener<handle>* r, int) {
            r->upload_callback_ = callback;
            *tag = mega_->register_file(callback);
            auto upload = new FileUpload;
            upload->listener_ = r;
            upload->size_ = callback->size();
            upload->h = node->nodehandle;
            upload->name = filename;
            upload->localname = std::to_string(*tag);
            mega_->client()->startxfer(PUT, upload);
            mega_->exec();
          },
          [=](EitherError<handle> e) {
            auto lock = mega_->lock();
            mega_->remove_file(*tag);
            if (e.left()) return r->done(e.left());
            if (*e.right() == 0)
              r->done(
                  Error{IHttpRequest::Failure, util::Error::NODE_NOT_FOUND});
            else
              r->done(toItem(mega_->client()->nodebyhandle(*e.right())));
          });
    });
  };
  return std::make_shared<Request<EitherError<IItem>>>(
             shared_from_this(), [=](EitherError<IItem> e) { cb->done(e); },
             resolver)
      ->run();
}

ICloudProvider::DeleteItemRequest::Pointer MegaNz::deleteItemAsync(
    IItem::Pointer item, DeleteItemCallback callback) {
  auto resolver = [=](Request<EitherError<void>>::Pointer r) {
    ensureAuthorized<EitherError<void>>(r, [=] {
      auto lock = mega_->lock();
      auto node = this->node(item->id());
      if (!node) {
        r->done(Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
      } else {
        r->make_subrequest<MegaNz>(&MegaNz::make_request<error>, Type::DELETE,
                                   [=](Listener<error>*, int) {
                                     mega_->client()->unlink(node, false);
                                     mega_->exec();
                                   },
                                   [=](EitherError<error> e) {
                                     if (e.left())
                                       r->done(e.left());
                                     else
                                       r->done(nullptr);
                                   });
      }
    });
  };
  return std::make_shared<Request<EitherError<void>>>(shared_from_this(),
                                                      callback, resolver)
      ->run();
}

ICloudProvider::CreateDirectoryRequest::Pointer MegaNz::createDirectoryAsync(
    IItem::Pointer parent, const std::string& name,
    CreateDirectoryCallback callback) {
  auto resolver = [=](Request<EitherError<IItem>>::Pointer r) {
    ensureAuthorized<EitherError<IItem>>(r, [=] {
      auto lock = mega_->lock();
      auto parent_node = this->node(parent->id());
      if (!parent_node)
        return r->done(
            Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
      r->make_subrequest<MegaNz>(
          &MegaNz::make_request<handle>, Type::MKDIR,
          [=](Listener<handle>*, int) {
            NewNode folder;
            folder.source = NEW_NODE;
            folder.type = FOLDERNODE;
            folder.nodehandle = 0;
            folder.parenthandle = UNDEF;

            SymmCipher key;
            uint8_t buf[FOLDERNODEKEYLENGTH];
            PrnGen::genblock(buf, FOLDERNODEKEYLENGTH);
            folder.nodekey.assign(reinterpret_cast<char*>(buf),
                                  FOLDERNODEKEYLENGTH);
            key.setkey(buf);

            AttrMap attrs;
            attrs.map['n'] = name;
            std::string attr_str;
            attrs.getjson(&attr_str);
            folder.attrstring = new std::string;
            mega_->client()->makeattr(&key, folder.attrstring,
                                      attr_str.c_str());
            mega_->client()->putnodes(parent_node->nodehandle, &folder, 1);
            mega_->exec();
          },
          [=](EitherError<handle> e) {
            if (e.left()) return r->done(e.left());
            auto lock = mega_->lock();
            auto node = mega_->client()->nodebyhandle(*e.right());
            r->done(toItem(node));
          });
    });
  };
  return std::make_shared<Request<EitherError<IItem>>>(shared_from_this(),
                                                       callback, resolver)
      ->run();
}

ICloudProvider::MoveItemRequest::Pointer MegaNz::moveItemAsync(
    IItem::Pointer source, IItem::Pointer destination,
    MoveItemCallback callback) {
  auto resolver = [=](Request<EitherError<IItem>>::Pointer r) {
    ensureAuthorized<EitherError<IItem>>(r, [=] {
      auto lock = mega_->lock();
      auto source_node = this->node(source->id());
      auto destination_node = this->node(destination->id());
      if (source_node && destination_node) {
        r->make_subrequest<MegaNz>(
            &MegaNz::make_request<handle>, Type::MOVE,
            [=](Listener<handle>*, int) {
              mega_->client()->rename(source_node, destination_node);
              mega_->exec();
            },
            [=](EitherError<handle> e) {
              if (e.left()) return r->done(e.left());
              auto lock = mega_->lock();
              auto node = this->mega_->client()->nodebyhandle(*e.right());
              r->done(toItem(node));
            });
      } else {
        r->done(Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
      }
    });
  };
  return std::make_shared<Request<EitherError<IItem>>>(shared_from_this(),
                                                       callback, resolver)
      ->run();
}

ICloudProvider::RenameItemRequest::Pointer MegaNz::renameItemAsync(
    IItem::Pointer item, const std::string& name, RenameItemCallback callback) {
  auto resolver = [=](Request<EitherError<IItem>>::Pointer r) {
    ensureAuthorized<EitherError<IItem>>(r, [=] {
      auto lock = mega_->lock();
      auto node = this->node(item->id());
      if (node) {
        r->make_subrequest<MegaNz>(&MegaNz::make_request<handle>, Type::RENAME,
                                   [=](Listener<handle>*, int) {
                                     node->attrs.map['n'] = name;
                                     mega_->client()->setattr(node);
                                     mega_->exec();
                                   },
                                   [=](EitherError<handle> e) {
                                     auto lock = mega_->lock();
                                     if (e.left()) return r->done(e.left());
                                     r->done(toItem(node));
                                   });
      } else
        r->done(Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
    });
  };
  return std::make_shared<Request<EitherError<IItem>>>(shared_from_this(),
                                                       callback, resolver)
      ->run();
}

ICloudProvider::ListDirectoryPageRequest::Pointer
MegaNz::listDirectoryPageAsync(IItem::Pointer item, const std::string&,
                               ListDirectoryPageCallback complete) {
  auto resolver = [=](Request<EitherError<PageData>>::Pointer r) {
    ensureAuthorized<EitherError<PageData>>(r, [=] {
      auto lock = mega_->lock();
      auto node = this->node(item->id());
      if (!node) {
        r->done(Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
        return;
      }
      IItem::List result;
      for (auto d : node->children) {
        auto item = toItem(d);
        result.push_back(item);
      }
      r->done(PageData{result, ""});
    });
  };
  return std::make_shared<Request<EitherError<PageData>>>(shared_from_this(),
                                                          complete, resolver)
      ->run();
}

ICloudProvider::GeneralDataRequest::Pointer MegaNz::getGeneralDataAsync(
    GeneralDataCallback callback) {
  auto resolver = [=](Request<EitherError<GeneralData>>::Pointer r) {
    ensureAuthorized<EitherError<GeneralData>>(r, [=] {
      r->make_subrequest<MegaNz>(
          &MegaNz::make_request<GeneralData>, Type::GENERAL_DATA,
          [=](Listener<GeneralData>*, int) {
            auto lock = mega_->lock();
            mega_->client()->getaccountdetails(new AccountDetails, true, false,
                                               false, false, false, false);
            mega_->exec();
          },
          [=](EitherError<GeneralData> e) {
            if (e.left()) return r->done(e.left());
            auto result = *e.right();
            result.username_ =
                credentialsFromString(token())["username"].asString();
            r->done(result);
          });
    });
  };
  return std::make_shared<Request<EitherError<GeneralData>>>(shared_from_this(),
                                                             callback, resolver)
      ->run();
}

std::function<void(Request<EitherError<void>>::Pointer)>
MegaNz::downloadResolver(IItem::Pointer item, IDownloadFileCallback* callback,
                         Range range) {
  return [=](Request<EitherError<void>>::Pointer r) {
    ensureAuthorized<EitherError<void>>(r, [=] {
      auto lock = mega_->lock();
      auto node = this->node(item->id());
      if (!node)
        return r->done(
            Error{IHttpRequest::NotFound, util::Error::NODE_NOT_FOUND});
      r->make_subrequest<MegaNz>(
          &MegaNz::make_request<error>, Type::READ,
          [=](Listener<error>* r, int tag) {
            r->download_callback_ = callback;
            r->total_bytes_ =
                range.size_ == Range::Full
                    ? static_cast<uint64_t>(node->size) - range.start_
                    : range.size_;
            mega_->client()->pread(
                node, range.start_, r->total_bytes_,
                reinterpret_cast<void*>(static_cast<uintptr_t>(tag)));
            mega_->exec();
          },
          [=](EitherError<error> e) {
            if (e.left())
              r->done(e.left());
            else
              r->done(nullptr);
          });
    });
  };
}

void MegaNz::login(Request<EitherError<void>>::Pointer r,
                   AuthorizeRequest::AuthorizeCompleted complete) {
  auto data = credentialsFromString(token());
  std::string mail = data["username"].asString();
  std::string private_key = data["password"].asString();
  auto key = util::from_base64(private_key);
  auto session_auth_callback = [=](EitherError<error> e) {
    if (!e.left() && *e.right() == API_OK) return complete(nullptr);
    auto lock = mega_->lock();
    r->make_subrequest<MegaNz>(
        &MegaNz::make_request<error>, Type::LOGIN,
        [=](Listener<error>*, int) {
          auto lock = mega_->lock();
          mega_->client()->login(mail.c_str(), (uint8_t*)(key.c_str()));
          mega_->exec();
        },
        [=](EitherError<error> e) {
          if (e.left()) return complete(e.left());
          if (*e.right() != 0)
            return complete(Error{*e.right(), error_description(*e.right())});
          {
            auto lock1 = auth_lock();
            auto lock2 = mega_->lock();
            char buffer[HASH_BUFFER_SIZE];
            auto length = mega_->client()->dumpsession((uint8_t*)buffer,
                                                       HASH_BUFFER_SIZE);
            auth()->access_token()->token_ =
                util::to_base64(std::string(buffer, length));
          }
          complete(nullptr);
        });
  };
  r->make_subrequest<MegaNz>(&MegaNz::make_request<error>, Type::LOGIN,
                             [=](Listener<error>*, int) {
                               auto lock = mega_->lock();
                               auto session = util::from_base64(access_token());
                               mega_->client()->login((uint8_t*)session.c_str(),
                                                      session.size());
                               mega_->exec();
                             },
                             session_auth_callback);
}

std::string MegaNz::passwordHash(const std::string& password) const {
  auto lock = mega_->lock();
  uint8_t buffer[HASH_BUFFER_SIZE];
  mega_->client()->pw_key(password.c_str(), buffer);
  return util::to_base64(std::string(reinterpret_cast<const char*>(buffer)));
}

IItem::Pointer MegaNz::toItem(Node* node) {
  auto item = util::make_unique<Item>(
      node->displayname(), std::to_string(node->nodehandle),
      node->type == FILENODE ? node->size : IItem::UnknownSize,
      node->type == FILENODE ? std::chrono::system_clock::time_point(
                                   std::chrono::seconds(node->ctime))
                             : IItem::UnknownTimeStamp,
      node->type == FILENODE ? IItem::FileType::Unknown
                             : IItem::FileType::Directory);
  item->set_url(defaultFileDaemonUrl(*item, node->size));
  return item;
}

template <class T>
std::shared_ptr<IRequest<EitherError<T>>> MegaNz::make_request(
    Type type, std::function<void(Listener<T>*, int)> init,
    GenericCallback<EitherError<T>> c) {
  auto r = std::make_shared<Listener<T>>(c);
  auto tag = mega_->register_callback(type, r);
  init(r.get(), tag);
  return r;
}

template <class T>
void MegaNz::ensureAuthorized(typename Request<T>::Pointer r,
                              std::function<void()> on_success) {
  auto f = [=](EitherError<void> e) {
    if (e.left())
      r->done(e.left());
    else
      on_success();
  };
  if (!authorized_)
    r->reauthorize(f);
  else
    f(nullptr);
}

IAuth::Token::Pointer MegaNz::authorizationCodeToToken(
    const std::string& code) const {
  auto data = credentialsFromString(code);
  IAuth::Token::Pointer token = util::make_unique<IAuth::Token>();
  Json::Value json;
  json["username"] = data["username"].asString();
  json["password"] = passwordHash(data["password"].asString());
  token->token_ = credentialsToString(json);
  token->refresh_token_ = token->token_;
  return token;
}

std::string MegaNz::Auth::authorizeLibraryUrl() const {
  return redirect_uri() + "/login?state=" + state();
}

IHttpRequest::Pointer MegaNz::Auth::exchangeAuthorizationCodeRequest(
    std::ostream&) const {
  return nullptr;
}

IHttpRequest::Pointer MegaNz::Auth::refreshTokenRequest(std::ostream&) const {
  return nullptr;
}

IAuth::Token::Pointer MegaNz::Auth::exchangeAuthorizationCodeResponse(
    std::istream&) const {
  return nullptr;
}

IAuth::Token::Pointer MegaNz::Auth::refreshTokenResponse(std::istream&) const {
  return nullptr;
}

}  // namespace cloudstorage

#endif  // WITH_MEGA
