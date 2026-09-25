#include "jobs.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "log.hpp"

namespace {

// 想定外の例外をログに出す（ここに来るのは不具合なので、原因を追えるように書き残す）。
void log_unexpected(const char* where, const std::exception* e) {
    applog::add(std::string { "内部エラー（" } + where + "）: " + (e ? e->what() : "不明な例外"));
}

}    // namespace

void JobQueue::launch(JobWork work, JobApply on_error) {
    jobs_.push_back(std::async(std::launch::async,
                               [work = std::move(work), on_error = std::move(on_error)]() -> JobApply {
                                   try {
                                       return work();
                                   } catch (const std::exception& e) {
                                       log_unexpected("ワーカー", &e);
                                   } catch (...) {
                                       log_unexpected("ワーカー", nullptr);
                                   }
                                   return on_error;
                               }));
}

bool JobQueue::try_launch(JobWork work, JobApply on_error) {
    if (busy()) return false;
    launch(std::move(work), std::move(on_error));
    return true;
}

void JobQueue::poll(App& app) {
    // 完了したものを先に取り出してから適用する。適用処理が新しいジョブを launch すると
    // jobs_ に追加されてイテレータが無効になるため、走査中には呼ばない。
    std::vector<JobApply> done;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        done.push_back(it->get());    // ワーカー側で例外を捕まえているので get は投げない
        it = jobs_.erase(it);
    }
    for (JobApply& apply : done) {
        if (!apply) continue;
        try {
            apply(app);
        } catch (const std::exception& e) {
            log_unexpected("結果の反映", &e);
        } catch (...) {
            log_unexpected("結果の反映", nullptr);
        }
    }
}
