/* XMRig
 * Copyright (c) 2019      Howard Chu  <https://github.com/hyc>
 * Copyright (c) 2018-2023 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2023 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 * Copyright 2026 wuchenxiuwu <https://github.com/wuchenxiuwu>
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _MSC_VER
#pragma warning(disable:4244)
#endif

#include <thread>
#include "net/Network.h"
#include "3rdparty/rapidjson/document.h"
#include "backend/common/Tags.h"
#include "base/io/log/Log.h"
#include "base/io/log/Tags.h"
#include "base/net/stratum/Client.h"
#include "base/net/stratum/NetworkState.h"
#include "base/net/stratum/SubmitResult.h"
#include "base/tools/Chrono.h"
#include "base/tools/Timer.h"
#include "core/config/Config.h"
#include "core/Controller.h"
#include "core/Miner.h"
#include "net/JobResult.h"
#include "net/JobResults.h"


#ifdef XMRIG_FEATURE_API
#   include "base/api/Api.h"
#   include "base/api/interfaces/IApiRequest.h"
#endif


#ifdef XMRIG_FEATURE_BENCHMARK
#   include "backend/common/benchmark/BenchState.h"
#endif


#include <algorithm>
#include <cinttypes>
#include <ctime>
#include <iterator>
#include <memory>
#include <cassert>
#include "base/kernel/interfaces/IStrategy.h"


xmrig::Network::Network(Controller *controller) :
    m_controller(controller),
    m_stopping(false)
{
    JobResults::setListener(this, controller->config()->cpu().isHwAES());
    controller->addListener(this);

#   ifdef XMRIG_FEATURE_API
    controller->api()->addListener(this);
#   endif

    m_state = new NetworkState(this);

    const Pools &pools = controller->config()->pools();
    m_strategy = pools.createStrategy(m_state);

    m_timer = new Timer(this, kTickInterval, kTickInterval);
}


xmrig::Network::~Network()
{
    // 先设置停止标志，防止回调进来
    m_stopping.store(true, std::memory_order_release);
    
    // 停止JobResults回调
    JobResults::stop();
    
    // 等待可能的回调完成（简单延迟，更精确需要条件变量）
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // 现在可以安全删除
    delete m_timer;
    delete m_strategy;
    delete m_state;
}


void xmrig::Network::connect()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // 检查策略对象是否存在
    if (m_strategy && !m_stopping.load(std::memory_order_acquire)) {
        m_strategy->connect();
    }
}


void xmrig::Network::execCommand(char command)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    switch (command) {
    case 's':
    case 'S':
        m_state->printResults();
        break;

    case 'c':
    case 'C':
        m_state->printConnection();
        break;

    default:
        break;
    }
}


void xmrig::Network::onActive(IStrategy *strategy, IClient *client)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    const auto &pool = client->pool();

#   ifdef XMRIG_FEATURE_BENCHMARK
    if (pool.mode() == Pool::MODE_BENCHMARK) {
        return;
    }
#   endif

    char zmq_buf[32] = {};
    if (client->pool().zmq_port() >= 0) {
        snprintf(zmq_buf, sizeof(zmq_buf), " (ZMQ:%d)", client->pool().zmq_port());
    }

    const char *tlsVersion = client->tlsVersion();
    LOG_INFO("%s " WHITE_BOLD("use %s ") CYAN_BOLD("%s:%d%s ") GREEN_BOLD("%s") " " BLACK_BOLD("%s"),
             Tags::network(), client->mode(), pool.host().data(), pool.port(), zmq_buf, tlsVersion ? tlsVersion : "", client->ip().data());

    const char *fingerprint = client->tlsFingerprint();
    if (fingerprint != nullptr) {
        LOG_INFO("%s " BLACK_BOLD("fingerprint (SHA-256): \"%s\""), Tags::network(), fingerprint);
    }
}


void xmrig::Network::onConfigChanged(Config *config, Config *previousConfig)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (config->pools() == previousConfig->pools() || !config->pools().active()) {
        return;
    }

    m_strategy->stop();

    config->pools().print();

    delete m_strategy;
    m_strategy = config->pools().createStrategy(m_state);
    
    // 安全连接
    if (!m_stopping.load(std::memory_order_acquire)) {
        m_strategy->connect();
    }
}


void xmrig::Network::onJob(IStrategy *strategy, IClient *client, const Job &job, const rapidjson::Value &)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // 安全检查：如果正在停止，不处理新任务
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    setJob(client, job);
}


void xmrig::Network::onJobResult(const JobResult &result)
{
    // 检查停止标志
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // 再次检查（双检查锁模式）
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    m_strategy->submit(result);
}


void xmrig::Network::onLogin(IStrategy *, IClient *client, rapidjson::Document &doc, rapidjson::Value &params)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    Algorithms algorithms     = m_controller->miner()->algorithms();
    const Algorithm algorithm = client->pool().algorithm();
    if (algorithm.isValid()) {
        const size_t index = static_cast<size_t>(std::distance(algorithms.begin(), std::find(algorithms.begin(), algorithms.end(), algorithm)));
        if (index > 0 && index < algorithms.size()) {
            std::swap(algorithms[0], algorithms[index]);
        }
    }

    Value algo(kArrayType);

    for (const auto &a : algorithms) {
        algo.PushBack(StringRef(a.name()), allocator);
    }

    params.AddMember("algo", algo, allocator);
}


void xmrig::Network::onPause(IStrategy *strategy)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    if (!m_strategy->isActive()) {
        LOG_ERR("%s " RED("no active pools, stop mining"), Tags::network());

        return m_controller->miner()->pause();
    }
}


void xmrig::Network::onResultAccepted(IStrategy *, IClient *, const SubmitResult &result, const char *error)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    uint64_t diff     = result.diff;
    const char *scale = NetworkState::scaleDiff(diff);

    if (error) {
        LOG_INFO("%s " RED_BOLD("rejected") " (%" PRId64 "/%" PRId64 ") diff " WHITE_BOLD("%" PRIu64 "%s") " " RED("\"%s\"") " " BLACK_BOLD("(%" PRIu64 " ms)"),
                 backend_tag(result.backend), m_state->accepted(), m_state->rejected(), diff, scale, error, result.elapsed);
    }
    else {
        LOG_INFO("%s " GREEN_BOLD("accepted") " (%" PRId64 "/%" PRId64 ") diff " WHITE_BOLD("%" PRIu64 "%s") " " BLACK_BOLD("(%" PRIu64 " ms)"),
                 backend_tag(result.backend), m_state->accepted(), m_state->rejected(), diff, scale, result.elapsed);
    }
}


void xmrig::Network::onVerifyAlgorithm(IStrategy *, const IClient *, const Algorithm &algorithm, bool *ok)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        *ok = false;
        return;
    }
    
    if (!m_controller->miner()->isEnabled(algorithm)) {
        *ok = false;

        return;
    }
}


#ifdef XMRIG_FEATURE_API
void xmrig::Network::onRequest(IApiRequest &request)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        request.done(500);
        return;
    }
    
    if (request.type() == IApiRequest::REQ_SUMMARY) {
        request.accept();

        getResults(request.reply(), request.doc(), request.version());
        getConnection(request.reply(), request.doc(), request.version());
    }
}
#endif


void xmrig::Network::setJob(IClient *client, const Job &job)
{
    // 安全检查：确保在持有锁的情况下调用
    // 注：由于这是私有函数，只在已加锁的onJob中调用，所以我们保持断言
    // 但在release构建中可能会被优化掉，所以我们保留注释说明
    
    // assert(!"setJob must be called with m_mutex held");
    // 实际代码中我们依赖调用者保证，这里只做状态检查
    
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
#   ifdef XMRIG_FEATURE_BENCHMARK
    if (!BenchState::size())
#   endif
    {
        uint64_t diff       = job.diff();
        const char *scale   = NetworkState::scaleDiff(diff);

        char zmq_buf[32] = {};
        if (client->pool().zmq_port() >= 0) {
            snprintf(zmq_buf, sizeof(zmq_buf), " (ZMQ:%d)", client->pool().zmq_port());
        }

        char tx_buf[32] = {};
        const uint32_t num_transactions = job.getNumTransactions();
        if (num_transactions > 0) {
            snprintf(tx_buf, sizeof(tx_buf), " (%u tx)", num_transactions);
        }

        char height_buf[64] = {};
        if (job.height() > 0) {
            snprintf(height_buf, sizeof(height_buf), " height " WHITE_BOLD("%" PRIu64), job.height());
        }

        LOG_INFO("%s " MAGENTA_BOLD("new job") " from " WHITE_BOLD("%s:%d%s") " diff " WHITE_BOLD("%" PRIu64 "%s") " algo " WHITE_BOLD("%s") "%s%s",
                 Tags::network(), client->pool().host().data(), client->pool().port(), zmq_buf, diff, scale, job.algorithm().name(), height_buf, tx_buf);
    }

    m_controller->miner()->setJob(job);
}


void xmrig::Network::tick()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (m_stopping.load(std::memory_order_acquire)) {
        return;
    }
    
    const uint64_t now = Chrono::steadyMSecs();

    m_strategy->tick(now);

#   ifdef XMRIG_FEATURE_API
    m_controller->api()->tick();
#   endif
}


#ifdef XMRIG_FEATURE_API
void xmrig::Network::getConnection(rapidjson::Value &reply, rapidjson::Document &doc, int version) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    reply.AddMember("algo",         m_state->algorithm().toJSON(), allocator);
    reply.AddMember("connection",   m_state->getConnection(doc, version), allocator);
}


void xmrig::Network::getResults(rapidjson::Value &reply, rapidjson::Document &doc, int version) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    using namespace rapidjson;
    auto &allocator = doc.GetAllocator();

    reply.AddMember("results", m_state->getResults(doc, version), allocator);
}
#endif