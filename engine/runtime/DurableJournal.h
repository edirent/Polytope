#pragma once
#include <boost/json.hpp>
#include <filesystem>
#include <string>
#include <vector>
namespace trading_engine::runtime {
std::string sha256(const std::string& bytes);
std::string file_sha256(const std::filesystem::path& path);
// One writer, POSIX advisory lock, hash chained JSONL, fsync before return.
// A damaged or torn tail is rejected, never silently truncated.
class DurableJournal {
public:
    explicit DurableJournal(const std::filesystem::path& path);
    ~DurableJournal();
    DurableJournal(const DurableJournal&) = delete;
    DurableJournal& operator=(const DurableJournal&) = delete;
    void append(const boost::json::object& event, bool durable = true);
    void flush();
    bool dirty() const { return dirty_; }
    const std::string& durable_head() const { return durable_head_; }
    void inject_fault(std::string mode) { fault_ = std::move(mode); }
    std::uint64_t last_write_ns() const { return write_ns_; }
    std::uint64_t last_fsync_ns() const { return fsync_ns_; }
    const std::vector<boost::json::object>& recovered() const { return recovered_; }
    const std::string& head() const { return head_; }
private:
    bool dirty_=false;
    std::string durable_head_=std::string(64,'0');
    std::string fault_;
    std::uint64_t write_ns_=0, fsync_ns_=0;
    int fd_ = -1;
    std::uint64_t seq_ = 0;
    std::string head_ = std::string(64, '0');
    std::vector<boost::json::object> recovered_;
};
void atomic_text(const std::filesystem::path& path, const std::string& text);
void atomic_json(const std::filesystem::path& path, const boost::json::object& value);
}
