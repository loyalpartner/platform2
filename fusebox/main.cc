// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sysexits.h>
#include <unistd.h>

#include <map>
#include <vector>

#include <base/bind.h>
#include <base/check.h>
#include <base/check_op.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/no_destructor.h>
#include <base/numerics/safe_conversions.h>
#include <base/posix/eintr_wrapper.h>

#include <brillo/daemons/dbus_daemon.h>
#include <brillo/syslog_logging.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/object_proxy.h>

#include "fusebox/dbus_adaptors/org.chromium.FuseBoxClient.h"
#include "fusebox/file_system.h"
#include "fusebox/file_system_fake.h"
#include "fusebox/fuse_file_handles.h"
#include "fusebox/fuse_frontend.h"
#include "fusebox/fuse_path_inodes.h"
#include "fusebox/make_stat.h"
#include "fusebox/proto_bindings/fusebox.pb.h"
#include "fusebox/util.h"

using base::CommandLine;

namespace {

static std::string* g_device;

void SetupDevice() {
  static std::string device;
  device = CommandLine::ForCurrentProcess()->GetSwitchValueASCII("storage");
  LOG_IF(INFO, !device.empty()) << "device: " << device;
  g_device = &device;
}

}  // namespace

namespace fusebox {

class FuseBoxClient : public org::chromium::FuseBoxClientInterface,
                      public org::chromium::FuseBoxClientAdaptor,
                      public FileSystem {
 public:
  FuseBoxClient(scoped_refptr<dbus::Bus> bus, FuseMount* fuse)
      : org::chromium::FuseBoxClientAdaptor(this),
        dbus_object_(nullptr, bus, dbus::ObjectPath(kFuseBoxClientPath)),
        bus_(bus),
        fuse_(fuse) {}
  FuseBoxClient(const FuseBoxClient&) = delete;
  FuseBoxClient& operator=(const FuseBoxClient&) = delete;
  virtual ~FuseBoxClient() = default;

  void RegisterDBusObjectsAsync(
      const brillo::dbus_utils::AsyncEventSequencer::CompletionAction& cb) {
    RegisterWithDBusObject(&dbus_object_);
    dbus_object_.RegisterAsync(cb);

    const auto path = dbus::ObjectPath(kFuseBoxServicePath);
    dbus_proxy_ = bus_->GetObjectProxy(kFuseBoxServiceName, path);
  }

  int StartFuseSession(base::OnceClosure stop_callback) {
    CHECK(stop_callback);

    fuse_frontend_.reset(new FuseFrontend(fuse_));
    FileSystem* fs = fuse_->fake ? CreateFakeFileSystem() : this;
    if (!fuse_frontend_->CreateFuseSession(fs, FileSystem::FuseOps()))
      return EX_SOFTWARE;

    fuse_frontend_->StartFuseSession(std::move(stop_callback));
    return EX_OK;
  }

  static FileSystem* CreateFakeFileSystem() {
    static base::NoDestructor<FileSystemFake> fake_file_system;
    return fake_file_system.get();
  }

  static InodeTable& GetInodeTable() {
    static base::NoDestructor<InodeTable> inode_table;
    return *inode_table;
  }

  static dbus::MethodCall GetFuseBoxServerMethod(
      const char* method = kFuseBoxOperationMethod) {
    return dbus::MethodCall(kFuseBoxServiceInterface, method);
  }

  template <typename Signature>
  void CallFuseBoxServerMethod(dbus::MethodCall* method_call,
                               base::OnceCallback<Signature> callback) {
    constexpr auto timeout = dbus::ObjectProxy::TIMEOUT_USE_DEFAULT;
    dbus_proxy_->CallMethod(method_call, timeout, std::move(callback));
  }

  void Init(void* userdata, struct fuse_conn_info*) override {
    VLOG(1) << "init";

    Node* root = GetInodeTable().Lookup(FUSE_ROOT_ID);
    struct stat root_stat = MakeTimeStat(S_IFDIR | 0770);
    root_stat = MakeStat(root->ino, root_stat);
    GetInodeTable().SetStat(root->ino, root_stat);

    CHECK(userdata) << "FileSystem (userdata) is required";
  }

  void GetAttr(std::unique_ptr<AttrRequest> request, ino_t ino) override {
    VLOG(1) << "getattr " << ino;

    if (request->IsInterrupted())
      return;

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "getattr " << ino;
      return;
    }

    if (node->ino == FUSE_ROOT_ID) {
      struct stat stat;
      CHECK(GetInodeTable().GetStat(node->ino, &stat));
      request->ReplyAttr(stat, kStatTimeoutSeconds);
      return;
    }

    dbus::MethodCall method = GetFuseBoxServerMethod();
    dbus::MessageWriter writer(&method);

    writer.AppendString("stat");
    auto item = *g_device + GetInodeTable().GetPath(node);
    writer.AppendString(item);

    auto stat_response =
        base::BindOnce(&FuseBoxClient::StatResponse, base::Unretained(this),
                       std::move(request), node->ino);
    CallFuseBoxServerMethod(&method, std::move(stat_response));
  }

  const double kStatTimeoutSeconds = 5.0;

  void StatResponse(std::unique_ptr<AttrRequest> request,
                    ino_t ino,
                    dbus::Response* response) {
    VLOG(1) << "getattr-resp " << ino;

    if (request->IsInterrupted())
      return;

    dbus::MessageReader reader(response);
    if (int error = GetResponseErrno(&reader, response)) {
      request->ReplyError(error);
      return;
    }

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "getattr-resp " << ino;
      return;
    }

    struct stat stat = GetServerStat(ino, &reader);
    request->ReplyAttr(stat, kStatTimeoutSeconds);
  }

  void Lookup(std::unique_ptr<EntryRequest> request,
              ino_t parent,
              const char* name) override {
    VLOG(1) << "lookup parent " << parent << "/" << name;

    if (request->IsInterrupted())
      return;

    Node* parent_node = GetInodeTable().Lookup(parent);
    if (!parent_node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "lookup parent " << parent;
      return;
    }

    dbus::MethodCall method = GetFuseBoxServerMethod();
    dbus::MessageWriter writer(&method);

    writer.AppendString("stat");
    const base::FilePath path(GetInodeTable().GetPath(parent_node));
    auto item = *g_device + path.Append(name).value();
    writer.AppendString(item);

    auto lookup_response =
        base::BindOnce(&FuseBoxClient::LookupResponse, base::Unretained(this),
                       std::move(request), parent, std::string(name));
    CallFuseBoxServerMethod(&method, std::move(lookup_response));
  }

  const double kEntryTimeoutSeconds = 5.0;

  void LookupResponse(std::unique_ptr<EntryRequest> request,
                      ino_t parent,
                      std::string name,
                      dbus::Response* response) {
    VLOG(1) << "lookup-resp parent " << parent << "/" << name;

    if (request->IsInterrupted())
      return;

    dbus::MessageReader reader(response);
    if (int error = GetResponseErrno(&reader, response)) {
      request->ReplyError(error);
      return;
    }

    Node* node = GetInodeTable().Ensure(parent, name.c_str());
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "lookup-resp parent " << parent << "/" << name;
      return;
    }

    fuse_entry_param entry = {0};
    entry.ino = static_cast<fuse_ino_t>(node->ino);
    entry.attr = GetServerStat(node->ino, &reader);
    entry.attr_timeout = kStatTimeoutSeconds;
    entry.entry_timeout = kEntryTimeoutSeconds;

    request->ReplyEntry(entry);
  }

  void OpenDir(std::unique_ptr<OpenRequest> request, ino_t ino) override {
    VLOG(1) << "opendir " << ino;

    if (request->IsInterrupted())
      return;

    if ((request->flags() & O_ACCMODE) != O_RDONLY) {
      errno = request->ReplyError(EACCES);
      PLOG(ERROR) << "opendir " << ino;
      return;
    }

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "opendir " << ino;
      return;
    }

    uint64_t handle = fusebox::OpenFile();
    request->ReplyOpen(handle);
  }

  void ReadDir(std::unique_ptr<DirEntryRequest> request,
               ino_t ino,
               off_t off) override {
    VLOG(1) << "readdir fh " << request->fh() << " off " << off;

    if (request->IsInterrupted())
      return;

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "readdir " << ino;
      return;
    }

    uint64_t handle = fusebox::GetFile(request->fh());
    if (!handle) {
      errno = request->ReplyError(EBADF);
      PLOG(ERROR) << "readdir fh " << request->fh();
      return;
    }

    auto it = readdir_.find(handle);
    if (it != readdir_.end()) {
      DirEntryResponse* response = it->second.get();
      response->Append(std::move(request));
      return;
    }

    auto& buffer = readdir_[handle];
    buffer.reset(new DirEntryResponse(node->ino, handle));
    buffer->Append(std::move(request));

    dbus::MethodCall method = GetFuseBoxServerMethod();
    dbus::MessageWriter writer(&method);

    writer.AppendString("readdir");
    auto item = *g_device + GetInodeTable().GetPath(node);
    writer.AppendString(item);
    writer.AppendUint64(handle);

    auto readdir_started =
        base::BindOnce(&FuseBoxClient::ReadDirStarted, base::Unretained(this),
                       node->ino, handle);
    CallFuseBoxServerMethod(&method, std::move(readdir_started));
  }

  void ReadDirStarted(ino_t ino, uint64_t handle, dbus::Response* response) {
    VLOG(1) << "readdir-resp fh " << handle;

    dbus::MessageReader reader(response);
    int error = GetResponseErrno(&reader, response);
    if (!error)
      return;

    auto it = readdir_.find(handle);
    if (it != readdir_.end()) {
      DirEntryResponse* response = it->second.get();
      response->Append(error);
    }
  }

  void ReadDirResponse(uint64_t handle,
                       int32_t file_error,
                       const std::vector<uint8_t>& list,
                       bool has_more) override {
    VLOG(1) << "readdir-resp fh " << handle;

    auto it = readdir_.find(handle);
    if (it == readdir_.end())
      return;

    DirEntryResponse* response = it->second.get();
    if (file_error) {
      errno = response->Append(FileErrorToErrno(file_error));
      PLOG(ERROR) << "readdir-resp [" << file_error << "]";
      return;
    }

    const ino_t parent = response->parent();
    if (!GetInodeTable().Lookup(parent)) {
      response->Append(errno);
      PLOG(ERROR) << "readdir-resp parent " << parent;
      return;
    }

    fusebox::DirEntryListProto proto;
    std::vector<fusebox::DirEntry> entries;
    CHECK(proto.ParseFromArray(list.data(), list.size()));

    for (const auto& item : proto.entries()) {
      const char* name = item.name().c_str();
      if (Node* node = GetInodeTable().Ensure(parent, name)) {
        mode_t mode = item.is_directory() ? S_IFDIR | 0770 : S_IFREG | 0770;
        entries.push_back({node->ino, item.name(), MakeStatModeBits(mode)});
      } else {
        response->Append(errno);
        PLOG(ERROR) << "parent ino: " << parent << " name: " << item.name();
        return;
      }
    }

    response->Append(std::move(entries), !has_more);
  }

  void ReleaseDir(std::unique_ptr<OkRequest> request, ino_t ino) override {
    VLOG(1) << "releasedir fh " << request->fh();

    if (request->IsInterrupted())
      return;

    if (!fusebox::GetFile(request->fh())) {
      errno = request->ReplyError(EBADF);
      PLOG(ERROR) << "releasedir fh " << request->fh();
      return;
    }

    fusebox::CloseFile(request->fh());
    readdir_.erase(request->fh());
    request->ReplyOk();
  }

  void Open(std::unique_ptr<OpenRequest> request, ino_t ino) override {
    VLOG(1) << "open " << ino;

    if (request->IsInterrupted())
      return;

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "open " << ino;
      return;
    }

    dbus::MethodCall method = GetFuseBoxServerMethod();
    dbus::MessageWriter writer(&method);

    writer.AppendString("open");
    auto item = *g_device + GetInodeTable().GetPath(node);
    writer.AppendString(item);
    VLOG(1) << "open flags " << OpenFlagsToString(request->flags());
    writer.AppendInt32(request->flags() & O_ACCMODE);

    auto open_response =
        base::BindOnce(&FuseBoxClient::OpenResponse, base::Unretained(this),
                       std::move(request), node->ino);
    CallFuseBoxServerMethod(&method, std::move(open_response));
  }

  void OpenResponse(std::unique_ptr<OpenRequest> request,
                    ino_t ino,
                    dbus::Response* response) {
    VLOG(1) << "open-resp " << ino;

    if (request->IsInterrupted())
      return;

    dbus::MessageReader reader(response);
    if (int error = GetResponseErrno(&reader, response)) {
      request->ReplyError(error);
      return;
    }

    base::ScopedFD fd;
    reader.PopFileDescriptor(&fd);

    if (!GetInodeTable().Lookup(ino)) {
      request->ReplyError(errno);
      PLOG(ERROR) << "open-resp " << ino;
      return;
    }

    uint64_t handle = fusebox::OpenFile(std::move(fd));
    request->ReplyOpen(handle);
  }

  void Read(std::unique_ptr<BufferRequest> request,
            ino_t ino,
            size_t size,
            off_t off) override {
    VLOG(1) << "read fh " << request->fh() << " off " << off << " size "
            << size;

    if (request->IsInterrupted())
      return;

    if (size > SSIZE_MAX) {
      errno = request->ReplyError(EINVAL);
      PLOG(ERROR) << "read size";
      return;
    }

    Node* node = GetInodeTable().Lookup(ino);
    if (!node) {
      request->ReplyError(errno);
      PLOG(ERROR) << "read " << ino;
      return;
    }

    if (!fusebox::GetFile(request->fh())) {
      errno = request->ReplyError(EBADF);
      PLOG(ERROR) << "read fh " << request->fh();
      return;
    }

    int fd = fusebox::GetFileDescriptor(request->fh());
    if (fd != -1) {
      ReadFileDescriptor(std::move(request), ino, fd, size, off);
      return;
    }

    dbus::MethodCall method = GetFuseBoxServerMethod();
    dbus::MessageWriter writer(&method);

    writer.AppendString("read");
    auto item = *g_device + GetInodeTable().GetPath(node);
    writer.AppendString(item);
    writer.AppendInt64(base::strict_cast<int64_t>(off));
    writer.AppendInt32(base::saturated_cast<int32_t>(size));

    auto read_response =
        base::BindOnce(&FuseBoxClient::ReadResponse, base::Unretained(this),
                       std::move(request), node->ino, size, off);
    CallFuseBoxServerMethod(&method, std::move(read_response));
  }

  void ReadResponse(std::unique_ptr<BufferRequest> request,
                    ino_t ino,
                    size_t size,
                    off_t off,
                    dbus::Response* response) {
    VLOG(1) << "read-resp fh " << request->fh() << " off " << off << " size "
            << size;

    if (request->IsInterrupted())
      return;

    dbus::MessageReader reader(response);
    if (int error = GetResponseErrno(&reader, response)) {
      request->ReplyError(error);
      return;
    }

    if (!fusebox::GetFile(request->fh())) {
      errno = request->ReplyError(EBADF);
      PLOG(ERROR) << "read-resp fh " << request->fh();
      return;
    }

    const uint8_t* buf = nullptr;
    size_t length = 0;
    reader.PopArrayOfBytes(&buf, &length);

    request->ReplyBuffer(reinterpret_cast<const char*>(buf), length);
  }

  void ReadFileDescriptor(std::unique_ptr<BufferRequest> request,
                          ino_t ino,
                          int fd,
                          size_t size,
                          off_t off) {
    VLOG(1) << "read-fd fh " << request->fh() << " off " << off << " size "
            << size;

    DCHECK_LE(size, SSIZE_MAX);
    std::vector<char> buf(size);

    DCHECK_NE(-1, fd);
    ssize_t length = HANDLE_EINTR(pread(fd, buf.data(), size, off));
    if (length == -1) {
      request->ReplyError(errno);
      PLOG(ERROR) << "read-fd fh " << request->fh();
      return;
    }

    request->ReplyBuffer(buf.data(), length);
  }

  void Release(std::unique_ptr<OkRequest> request, ino_t ino) override {
    VLOG(1) << "release fh " << request->fh();

    if (request->IsInterrupted())
      return;

    if (!fusebox::GetFile(request->fh())) {
      errno = request->ReplyError(EBADF);
      PLOG(ERROR) << "release fh " << request->fh();
      return;
    }

    fusebox::CloseFile(request->fh());
    request->ReplyOk();
  }

  bool AttachStorage(brillo::ErrorPtr*, const std::string& name) override {
    VLOG(1) << "attach-storage " << name;

    Node* node = GetInodeTable().Ensure(FUSE_ROOT_ID, name.c_str());
    if (!node) {
      PLOG(ERROR) << "attach-storage " << name;
      return false;
    }

    if (!node->device) {
      node->device = ++device_;
      CHECK(node->device) << "device wrapped";
    }

    return true;
  }

  bool DetachStorage(brillo::ErrorPtr*, const std::string& name) override {
    VLOG(1) << "detach-storage " << name;

    Node* node = GetInodeTable().Lookup(FUSE_ROOT_ID, name.c_str());
    if (!node)
      return true;

    // TODO(crbug.com/1289493): remove all node->device nodes from inode table.
    CHECK(node->device);
    return false;
  }

 private:
  // Client D-Bus object.
  brillo::dbus_utils::DBusObject dbus_object_;

  // Server D-Bus proxy.
  scoped_refptr<dbus::ObjectProxy> dbus_proxy_;

  // D-Bus.
  scoped_refptr<dbus::Bus> bus_;

  // Device number allocator.
  dev_t device_ = 0;

  // Fuse mount: not owned.
  FuseMount* fuse_ = nullptr;

  // Fuse user-space frontend.
  std::unique_ptr<FuseFrontend> fuse_frontend_;

  // Active readdir requests.
  std::map<uint64_t, std::unique_ptr<DirEntryResponse>> readdir_;
};

class FuseBoxDaemon : public brillo::DBusServiceDaemon {
 public:
  explicit FuseBoxDaemon(FuseMount* fuse)
      : DBusServiceDaemon(kFuseBoxClientName), fuse_(fuse) {}
  FuseBoxDaemon(const FuseBoxDaemon&) = delete;
  FuseBoxDaemon& operator=(const FuseBoxDaemon&) = delete;
  ~FuseBoxDaemon() = default;

 protected:
  // brillo::DBusServiceDaemon overrides.

  void RegisterDBusObjectsAsync(
      brillo::dbus_utils::AsyncEventSequencer* sequencer) override {
    bus_->AssertOnDBusThread();

    client_.reset(new FuseBoxClient(bus_, fuse_));
    client_->RegisterDBusObjectsAsync(
        sequencer->GetHandler("D-Bus register async failed", true));
  }

  int OnEventLoopStarted() override {
    bus_->AssertOnDBusThread();

    int ret = brillo::DBusServiceDaemon::OnEventLoopStarted();
    if (ret != EX_OK)
      return ret;

    auto quit = base::BindOnce(&Daemon::Quit, base::Unretained(this));
    return client_->StartFuseSession(std::move(quit));
  }

  void OnShutdown(int* exit_code) override {
    bus_->AssertOnDBusThread();

    DBusServiceDaemon::OnShutdown(exit_code);
    client_.reset();
  }

 private:
  // Fuse mount: not owned.
  FuseMount* fuse_ = nullptr;

  // Fuse user-space client.
  std::unique_ptr<FuseBoxClient> client_;
};

int Run(char** mountpoint, fuse_chan* chan, int foreground) {
  LOG(INFO) << "fusebox " << *mountpoint << " [" << getpid() << "]";

  FuseMount fuse = FuseMount(mountpoint, chan);
  fuse.debug = CommandLine::ForCurrentProcess()->HasSwitch("debug");
  fuse.fake = CommandLine::ForCurrentProcess()->HasSwitch("fake");
  fuse.opts = CommandLine::ForCurrentProcess()->GetSwitchValueASCII("ll");

  if (!foreground)
    LOG(INFO) << "fusebox fuse_daemonizing";
  fuse_daemonize(foreground);

  auto daemon = FuseBoxDaemon(&fuse);
  return daemon.Run();
}

}  // namespace fusebox

int main(int argc, char** argv) {
  CommandLine::Init(argc, argv);
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);
  SetupDevice();

  fuse_args args = FUSE_ARGS_INIT(argc, argv);
  char* mountpoint = nullptr;

  int foreground = 0;
  if (fuse_parse_cmdline(&args, &mountpoint, nullptr, &foreground) == -1) {
    PLOG(ERROR) << "fuse_parse_cmdline() failed";
    return EX_USAGE;
  }

  if (!mountpoint) {
    LOG(ERROR) << "fuse_parse_cmdline() mountpoint expected";
    return ENODEV;
  }

  fuse_chan* chan = fuse_mount(mountpoint, &args);
  if (!chan) {
    PLOG(ERROR) << "fuse_mount() [" << mountpoint << "] failed";
    return ENODEV;
  }

  int exit_code = fusebox::Run(&mountpoint, chan, foreground);

  if (fuse_session* session = fuse_chan_session(chan))
    fuse_session_destroy(session);
  fuse_opt_free_args(&args);

  if (!mountpoint) {  // Kernel removed the FUSE mountpoint: umount(8).
    exit_code = ENODEV;
  } else {
    fuse_unmount(mountpoint, nullptr);
  }

  errno = exit_code;
  PLOG_IF(ERROR, exit_code) << "fusebox exiting";
  return exit_code;
}
