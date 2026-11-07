#include "engine/runtime/DurableJournal.h"
#include "engine/runtime/LatencyMetrics.h"
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <cerrno>
namespace trading_engine::runtime {
namespace {
void write_all(int fd, const std::string& data) {
    std::size_t n = 0;
    while (n < data.size()) {
        auto count = ::write(fd, data.data()+n, data.size()-n);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("durable write failed");
        n += static_cast<std::size_t>(count);
    }
    if (::fsync(fd) != 0) throw std::runtime_error("fsync failed");
}
void sync_directory(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("directory open failed");
    int r = ::fsync(fd); ::close(fd);
    if (r != 0) throw std::runtime_error("directory fsync failed");
}
std::string digest(EVP_MD_CTX* ctx) {
    std::array<unsigned char,32> bytes{}; unsigned int n = 0;
    if (EVP_DigestFinal_ex(ctx, bytes.data(), &n) != 1 || n != 32) throw std::runtime_error("sha256 failed");
    const char* hex="0123456789abcdef"; std::string out;
    for (auto b:bytes) { out+=hex[b>>4]; out+=hex[b&15]; }
    return out;
}
}
std::string sha256(const std::string& data) {
    auto* ctx=EVP_MD_CTX_new(); if (!ctx) throw std::runtime_error("sha256 allocation");
    EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr); EVP_DigestUpdate(ctx,data.data(),data.size());
    auto result=digest(ctx); EVP_MD_CTX_free(ctx); return result;
}
std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error("cannot hash input: "+path.string());
    auto* ctx=EVP_MD_CTX_new(); if(!ctx) throw std::runtime_error("sha256 allocation");
    EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr); std::array<char,65536> buf{};
    while(in) { in.read(buf.data(),buf.size()); EVP_DigestUpdate(ctx,buf.data(),in.gcount()); }
    auto result=digest(ctx); EVP_MD_CTX_free(ctx); return result;
}
DurableJournal::DurableJournal(const std::filesystem::path& path) {
    fd_=::open(path.c_str(),O_CREAT|O_RDWR|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd_<0) throw std::runtime_error("journal open failed");
    try {
        if(::flock(fd_,LOCK_EX|LOCK_NB)!=0) throw std::runtime_error("journal already owned by another runtime");
        sync_directory(path.parent_path());
        std::ifstream input(path,std::ios::binary);
        std::string line;
        while(std::getline(input,line)) {
            if(input.eof()) throw std::runtime_error("torn journal tail; operator repair required");
            auto row=boost::json::parse(line).as_object();
            auto body=boost::json::serialize(row.at("event"));
            auto seq=row.at("seq").to_number<std::uint64_t>();
            auto hash=sha256(head_+"\n"+std::to_string(seq)+"\n"+body);
            if(seq!=seq_+1 || row.at("prev").as_string()!=head_ || row.at("sha256").as_string()!=hash)
                throw std::runtime_error("journal hash or sequence mismatch");
            seq_=seq; head_=hash; recovered_.push_back(row.at("event").as_object());
            durable_head_=head_;
        }
    } catch(...) { ::close(fd_); fd_=-1; throw; }
}
DurableJournal::~DurableJournal() { if(fd_>=0) ::close(fd_); }
void DurableJournal::append(const boost::json::object& event, bool durable) {
    const auto seq=seq_+1;
    auto hash=sha256(head_+"\n"+std::to_string(seq)+"\n"+boost::json::serialize(event));
    boost::json::object row{{"seq",seq},{"prev",head_},{"event",event},{"sha256",hash}};
    const auto bytes=boost::json::serialize(row)+"\n";
    const auto start=monotonic_ns();
    if(fault_=="ENOSPC") throw std::runtime_error("journal ENOSPC injected before durable boundary");
    if(fault_=="PARTIAL_WRITE") {
        (void)::write(fd_,bytes.data(),bytes.size()/2); (void)::fsync(fd_);
        throw std::runtime_error("journal partial write injected");
    }
    std::size_t done=0;
    while(done<bytes.size()) {
        auto n=::write(fd_,bytes.data()+done,bytes.size()-done);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) throw std::runtime_error("journal write failed");
        done+=static_cast<std::size_t>(n);
    }
    const auto written=monotonic_ns();write_ns_=written-start;fsync_ns_=0;
    seq_=seq;head_=hash;dirty_=true;
    if(durable)flush();
}
void DurableJournal::flush() {
    if(!dirty_)return;
    if(fault_=="ENOSPC")throw std::runtime_error("journal ENOSPC injected before fsync");
    const auto began=monotonic_ns();int result;
    do { result=::fsync(fd_); } while(result!=0 && errno==EINTR);
    fsync_ns_=monotonic_ns()-began;
    if(result!=0)throw std::runtime_error("journal fsync failed");
    dirty_=false;durable_head_=head_;
}

void atomic_text(const std::filesystem::path& path, const std::string& text) {
    auto tmp=path; tmp+=".tmp";
    int fd=::open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) throw std::runtime_error("snapshot open failed");
    try { write_all(fd,text); ::close(fd); }
    catch(...) {::close(fd);throw;}
    std::filesystem::rename(tmp,path); sync_directory(path.parent_path());
}
void atomic_json(const std::filesystem::path& path, const boost::json::object& value) {
    atomic_text(path, boost::json::serialize(value)+"\n");
}

}
