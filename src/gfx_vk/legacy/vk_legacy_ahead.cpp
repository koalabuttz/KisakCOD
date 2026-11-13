// Vulkan Legacy adapter: the pipeline list and the workers that build its
// pipelines ahead of the draws that need them. See vk_legacy.h, "Pipelines
// built ahead".
//
// Everything outside Worker and BuildJob runs on the device's thread (the one
// the adapter's calls come from). The workers see only the queue, the jobs
// they take from it and the pixel modules they share, all under Ahead::lock,
// and the shaders, declaration and layout a job names, which never change
// while they live: the device thread waits for every job that names a shader
// or declaration before it destroys one (AheadForget).

#include "vk_legacy_internal.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace gfx_vk::legacy
{
namespace
{
// ---- The list ------------------------------------------------------------------------------

constexpr char kMagic[8] = {'K', 'V', 'K', 'P', 'L', 'S', 'T', '1'};
constexpr uint32_t kListVersion = 3; // 2: ContentHash reads 8-byte words; 3: linear inputs in the key
constexpr uint32_t kMaxEntries = 16384;

// A pipeline as the list names it: its shaders and declaration by their
// contents, and the rest of its key as it is (ids zero).
struct StableKey
{
    uint64_t vs = 0, ps = 0, decl = 0;
    PipelineKey key{};
    bool operator==(const StableKey &other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};
static_assert(sizeof(StableKey) == 24 + sizeof(PipelineKey), "StableKey is hashed and written as bytes");
struct StableKeyHash
{
    size_t operator()(const StableKey &key) const { return static_cast<size_t>(ContentHash(&key, sizeof(key))); }
};

// An entry as the file holds it. age: launches since a draw last used it,
// counted only in launches that built it (its shaders were there).
struct FileEntry
{
    StableKey stable;
    uint8_t age = 0;
    uint8_t unused[7] = {};
};
static_assert(sizeof(FileEntry) == sizeof(StableKey) + 8, "FileEntry is written as bytes");

struct FileHeader
{
    char magic[8];
    uint32_t version;
    uint32_t entrySize;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t count;
    uint32_t checksum; // low half of ContentHash over the entries
};
static_assert(sizeof(FileHeader) == 32, "FileHeader is written as bytes");

struct Entry
{
    StableKey stable;
    uint8_t age = 0;
    bool drawn = false;     // a draw used it this launch
    bool buildable = false; // its shaders and declaration were there this launch
};

// ---- Jobs ----------------------------------------------------------------------------------

enum class JobState
{
    Queued,
    Building,
    Done,
};

struct Job
{
    PipelineKey key{};
    Shader *vs = nullptr;
    Shader *ps = nullptr;
    VertexDecl *decl = nullptr;
    Layout *layout = nullptr;
    bool dynamicStride = true;
    // The pixel module: one the shader kept when the job was queued, else
    // built from recipe (shared with other jobs through Ahead::modules).
    VkShaderModule fragment = VK_NULL_HANDLE;
    PixelModuleRecipe recipe;
    // Under Ahead::lock from here on.
    JobState state = JobState::Queued;
    // Results, written by the worker before it sets Done.
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t bindings = 0;     // Pipeline::bindings
    bool sharedModule = false; // fragment is in Ahead::modules
    bool builtModule = false;  // this job's worker built it
    double moduleMs = 0.0;
    std::string error;
    double ms = 0.0;
};

// A pixel module workers built, by pixel shader id and Shader::linked key,
// until the shader keeps it (kept) or goes.
struct SharedModule
{
    VkShaderModule module = VK_NULL_HANDLE;
    bool building = true;
    bool kept = false;
};
using ModuleKey = std::pair<uint32_t, LinkedKey>;
} // namespace

struct Ahead
{
    Core *core = nullptr;
    std::string path;
    bool on = false; // OptionalFeatures::pipelinesAhead
    std::vector<Entry> entries;
    std::unordered_map<StableKey, uint32_t, StableKeyHash> index;
    // Entries by the content hash of a shader or declaration they name.
    std::unordered_multimap<uint64_t, uint32_t> byContent;
    // The live shaders and declarations by content, oldest first: a list
    // entry is built for the newest.
    std::unordered_map<uint64_t, std::vector<Shader *>> shaders;
    std::unordered_map<uint64_t, std::vector<VertexDecl *>> decls;
    // The bytes last read from or written to writtenPath, to skip writing
    // them again unchanged.
    std::string writtenPath;
    std::vector<uint8_t> written;
    // Jobs not collected yet, by key; only the device thread changes the map.
    std::unordered_map<PipelineKey, std::unique_ptr<Job>, PipelineKeyHash> jobs;
    bool warnedFailure = false;

    std::mutex lock;
    std::condition_variable wake;     // workers: a job was queued, or stop
    std::condition_variable finished; // a job or a shared module is done
    std::deque<Job *> queue;
    std::vector<Job *> done;
    std::map<ModuleKey, SharedModule> modules;
    bool stop = false;
    std::vector<std::thread> workers;
};

uint64_t ContentHash(const void *data, size_t bytes, uint64_t hash)
{
    // FNV-1a over 8-byte words (shader bytecode is read at every start), then
    // the bytes left over.
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8)
    {
        uint64_t word;
        std::memcpy(&word, p + i, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; i < bytes; ++i)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash ^ (hash >> 29);
}

namespace
{
// MSVC rejects fopen as unsafe (C4996, an error under /WX).
FILE *OpenFile(const char *path, const char *mode)
{
#ifdef _MSC_VER
    FILE *file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

void GpuIds(const Device &device, uint32_t &vendor, uint32_t &id)
{
    VkPhysicalDeviceProperties properties{};
    device.vk->vkGetPhysicalDeviceProperties(PhysicalDeviceHandle(device.core), &properties);
    vendor = properties.vendorID;
    id = properties.deviceID;
}

StableKey StableFor(const PipelineKey &key, uint64_t vs, uint64_t ps, uint64_t decl)
{
    StableKey stable;
    stable.vs = vs;
    stable.ps = ps;
    stable.decl = decl;
    stable.key = key;
    stable.key.vs = stable.key.ps = stable.key.decl = 0;
    return stable;
}

uint32_t AddEntry(Ahead &ahead, const StableKey &stable, uint8_t age)
{
    const uint32_t at = static_cast<uint32_t>(ahead.entries.size());
    Entry entry;
    entry.stable = stable;
    entry.age = age;
    ahead.entries.push_back(entry);
    ahead.index.emplace(stable, at);
    ahead.byContent.emplace(stable.vs, at);
    if (stable.ps != stable.vs)
        ahead.byContent.emplace(stable.ps, at);
    ahead.byContent.emplace(stable.decl, at);
    return at;
}

void ClearList(Ahead &ahead)
{
    ahead.entries.clear();
    ahead.index.clear();
    ahead.byContent.clear();
}

// ---- Workers ---------------------------------------------------------------------------------

// The job's pixel module, built by this worker or another. Null on failure.
VkShaderModule SharedPixelModule(Ahead &ahead, Job &job)
{
    const ModuleKey key{job.ps->id, job.recipe.plain ? LinkedKey{} : job.recipe.moduleKey};
    {
        std::unique_lock<std::mutex> hold(ahead.lock);
        for (;;)
        {
            auto found = ahead.modules.find(key);
            if (found == ahead.modules.end())
            {
                ahead.modules.emplace(key, SharedModule{});
                break;
            }
            if (!found->second.building)
            {
                if (found->second.module)
                    return found->second.module;
                ahead.modules.erase(found); // that build failed: try again
                ahead.modules.emplace(key, SharedModule{});
                break;
            }
            ahead.finished.wait(hold);
        }
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint32_t> spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (BuildPixelSpirv(job.recipe, *job.ps, *job.vs, spirv, job.error))
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spirv.size() * sizeof(uint32_t);
        info.pCode = spirv.data();
        const Dispatch &vk = Functions(ahead.core);
        if (vk.vkCreateShaderModule(DeviceHandle(ahead.core), &info, nullptr, &module) != VK_SUCCESS)
        {
            module = VK_NULL_HANDLE;
            job.error = "vkCreateShaderModule failed";
        }
    }
    job.builtModule = true;
    job.moduleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard<std::mutex> hold(ahead.lock);
    SharedModule &shared = ahead.modules[key];
    shared.module = module;
    shared.building = false;
    ahead.finished.notify_all();
    return module;
}

void BuildJob(Ahead &ahead, Job &job)
{
    const auto start = std::chrono::steady_clock::now();
    VkShaderModule fragment = job.fragment;
    if (!fragment)
    {
        fragment = SharedPixelModule(ahead, job);
        job.sharedModule = fragment != VK_NULL_HANDLE;
    }
    if (fragment)
    {
        PipelineInfo info;
        MakePipelineInfo(info, job.key, *job.vs, fragment, *job.decl, *job.layout, job.dynamicStride);
        OffThreadPipeline result;
        job.bindings = info.bindingMask;
        job.pipeline = CreateGraphicsPipelineOffThread(ahead.core, info.info, result);
        if (!job.pipeline)
            job.error = "vkCreateGraphicsPipelines returned " + std::to_string(static_cast<int>(result.result));
    }
    job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void Worker(Ahead *ahead)
{
    for (;;)
    {
        Job *job = nullptr;
        {
            std::unique_lock<std::mutex> hold(ahead->lock);
            ahead->wake.wait(hold, [ahead] { return ahead->stop || !ahead->queue.empty(); });
            if (ahead->stop)
                return;
            job = ahead->queue.front();
            ahead->queue.pop_front();
            job->state = JobState::Building;
        }
        BuildJob(*ahead, *job);
        std::lock_guard<std::mutex> hold(ahead->lock);
        job->state = JobState::Done;
        ahead->done.push_back(job);
        ahead->finished.notify_all();
    }
}

// ---- The device's side ---------------------------------------------------------------------

// Hands what the workers have finished to the device: pipelines into
// Device::pipelines, pixel modules to their shaders.
void Collect(Device &device)
{
    Ahead &ahead = *device.ahead;
    std::vector<Job *> done;
    {
        std::lock_guard<std::mutex> hold(ahead.lock);
        done.swap(ahead.done);
    }
    for (Job *job : done)
    {
        if (job->builtModule)
        {
            ++device.stats.aheadPixelModules;
            device.stats.aheadPixelModuleMicros += static_cast<uint64_t>(job->moduleMs * 1000.0);
        }
        if (job->sharedModule)
        {
            std::lock_guard<std::mutex> hold(ahead.lock);
            SharedModule &shared = ahead.modules[{job->ps->id, job->recipe.plain ? LinkedKey{} : job->recipe.moduleKey}];
            if (!shared.kept && KeepPixelModule(device, *job->ps, job->recipe, shared.module))
                shared.kept = true;
        }
        if (job->pipeline)
        {
            device.stats.aheadMicros += static_cast<uint64_t>(job->ms * 1000.0);
            if (device.pipelines.count(job->key))
            {
                DestroyPipeline(device.core, job->pipeline); // a draw built it meanwhile
            }
            else
            {
                Pipeline *pipeline = AddPipeline(device, job->key, job->pipeline, job->layout, job->bindings,
                                                 *job->vs, *job->ps, *job->decl);
                pipeline->ahead = true;
                ++device.stats.pipelinesAhead;
            }
        }
        else if (!ahead.warnedFailure)
        {
            ahead.warnedFailure = true;
            Log(device, "a pipeline built ahead failed (shaders %08x/%08x): %s; it is built at its first draw",
                job->vs->translated.bytecodeHash, job->ps->translated.bytecodeHash, job->error.c_str());
        }
        ahead.jobs.erase(job->key);
    }
}

// Waits until no worker builds a job that names this shader or declaration
// and drops the queued ones; then collects.
void SettleJobsFor(Device &device, uint32_t id)
{
    Ahead &ahead = *device.ahead;
    std::vector<PipelineKey> dropped;
    {
        std::unique_lock<std::mutex> hold(ahead.lock);
        for (auto it = ahead.queue.begin(); it != ahead.queue.end();)
        {
            Job *job = *it;
            if (job->vs->id == id || job->ps->id == id || job->decl->id == id)
            {
                dropped.push_back(job->key);
                it = ahead.queue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        ahead.finished.wait(hold, [&] {
            for (const auto &entry : ahead.jobs)
            {
                const Job &job = *entry.second;
                if (job.state == JobState::Building && (job.vs->id == id || job.ps->id == id || job.decl->id == id))
                    return false;
            }
            return true;
        });
    }
    for (const PipelineKey &key : dropped)
        ahead.jobs.erase(key);
    Collect(device);
}

void StartWorkers(Ahead &ahead)
{
    // One worker for every three CPU threads, at most two, so the game's own
    // threads keep theirs, unless the core says (OptionalFeatures).
    const uint32_t threads = std::thread::hardware_concurrency();
    const uint32_t asked = Features(ahead.core).pipelineWorkers;
    const uint32_t count = asked ? std::min(asked, 8u) : std::max(1u, std::min(2u, threads / 3));
    for (uint32_t i = 0; i < count; ++i)
        ahead.workers.emplace_back(Worker, &ahead);
}

// Queues the entry's pipeline when its shaders and declaration are there, the
// device can build it as it stands, and nobody has built or queued it.
void TryQueue(Device &device, uint32_t at)
{
    Ahead &ahead = *device.ahead;
    if (!device.error.empty())
        return;
    Entry &entry = ahead.entries[at];
    const StableKey &stable = entry.stable;
    auto vs = ahead.shaders.find(stable.vs);
    auto ps = ahead.shaders.find(stable.ps);
    auto declared = ahead.decls.find(stable.decl);
    if (vs == ahead.shaders.end() || ps == ahead.shaders.end() || declared == ahead.decls.end())
        return;
    Shader &vertex = *vs->second.back();
    Shader &pixel = *ps->second.back();
    VertexDecl &decl = *declared->second.back();
    if (vertex.stage != ls::Stage::Vertex || pixel.stage != ls::Stage::Pixel)
        return;
    entry.buildable = true; // for its age, whether or not workers build it
    if (!ahead.on)
        return;
    // A key the device would not make (switches or the GPU changed since the
    // list was written) is left alone.
    const OptionalFeatures &features = Features(device.core);
    const PipelineKey &listed = stable.key;
    if ((listed.alphaCompare && !features.fixedAlphaTest) || (listed.depthSamplers && !features.fixedDepthCompare))
        return;
    bool strides = false;
    for (uint16_t stride : listed.strides)
        strides = strides || stride;
    if (strides && features.dynamicVertexStride)
        return;
    if (listed.depthFormat && listed.depthFormat != static_cast<uint32_t>(device.depthFormat))
        return;
    if (UsableSamples(&device, listed.samples) != listed.samples || listed.colorCount > kMaxRenderTargets)
        return;

    PipelineKey key = listed;
    key.vs = vertex.pipelineId;
    key.ps = pixel.pipelineId;
    key.decl = decl.pipelineId;
    if (device.pipelines.count(key) || ahead.jobs.count(key))
        return;
    Layout *layout = GetLayout(device, LayoutFor(vertex, pixel));
    if (!layout)
        return;
    std::unique_ptr<Job> job(new Job());
    job->key = key;
    job->vs = &vertex;
    job->ps = &pixel;
    job->decl = &decl;
    job->layout = layout;
    job->dynamicStride = features.dynamicVertexStride;
    job->fragment = FindPixelModule(device, pixel, vertex, key.depthSamplers, key.alphaCompare, key,
                                    job->recipe);
    if (ahead.workers.empty())
        StartWorkers(ahead);
    Job *queued = job.get();
    ahead.jobs.emplace(key, std::move(job));
    std::lock_guard<std::mutex> hold(ahead.lock);
    ahead.queue.push_back(queued);
    ahead.wake.notify_one();
}

void QueueFor(Device &device, uint64_t contentHash)
{
    const TimeInto timer(device.stats.aheadQueueMicros);
    Ahead &ahead = *device.ahead;
    const auto range = ahead.byContent.equal_range(contentHash);
    std::vector<uint32_t> found;
    for (auto it = range.first; it != range.second; ++it)
        found.push_back(it->second);
    for (uint32_t at : found)
        TryQueue(device, at);
}

// The list as the file holds it after this launch.
std::vector<uint8_t> ListBytes(const Device &device)
{
    const Ahead &ahead = *device.ahead;
    std::vector<FileEntry> kept;
    for (const Entry &entry : ahead.entries)
    {
        FileEntry out;
        out.stable = entry.stable;
        if (entry.drawn)
            out.age = 0;
        else if (entry.buildable)
            out.age = static_cast<uint8_t>(entry.age + 1);
        else
            out.age = entry.age;
        if (out.age < kPipelineListAge)
            kept.push_back(out);
    }
    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kListVersion;
    header.entrySize = sizeof(FileEntry);
    GpuIds(device, header.vendorID, header.deviceID);
    header.count = static_cast<uint32_t>(kept.size());
    header.checksum = static_cast<uint32_t>(ContentHash(kept.data(), kept.size() * sizeof(FileEntry)));
    std::vector<uint8_t> bytes(sizeof(header) + kept.size() * sizeof(FileEntry));
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!kept.empty())
        std::memcpy(bytes.data() + sizeof(header), kept.data(), kept.size() * sizeof(FileEntry));
    return bytes;
}

bool ReadList(Device &device, const char *path, std::string &message)
{
    Ahead &ahead = *device.ahead;
    ClearList(ahead);
    ahead.writtenPath.clear();
    ahead.written.clear();
    // Draws list their pipelines again into the new list.
    for (auto &entry : device.pipelines)
        entry.second.drawn = false;
    FILE *file = OpenFile(path, "rb");
    if (!file)
    {
        message = "no file";
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + got);
    const bool readError = std::ferror(file) != 0;
    std::fclose(file);
    FileHeader header{};
    if (readError || bytes.size() < sizeof(header))
    {
        message = "too short; starting empty";
        return false;
    }
    std::memcpy(&header, bytes.data(), sizeof(header));
    uint32_t vendor = 0, id = 0;
    GpuIds(device, vendor, id);
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kListVersion
        || header.entrySize != sizeof(FileEntry))
    {
        message = "another version; starting empty";
        return false;
    }
    if (header.vendorID != vendor || header.deviceID != id)
    {
        message = "made on another GPU; starting empty";
        return false;
    }
    if (header.count > kMaxEntries || bytes.size() != sizeof(header) + size_t(header.count) * sizeof(FileEntry)
        || static_cast<uint32_t>(ContentHash(bytes.data() + sizeof(header), bytes.size() - sizeof(header)))
               != header.checksum)
    {
        message = "damaged; starting empty";
        return false;
    }
    for (uint32_t i = 0; i < header.count; ++i)
    {
        FileEntry entry;
        std::memcpy(&entry, bytes.data() + sizeof(header) + size_t(i) * sizeof(FileEntry), sizeof(entry));
        if (!ahead.index.count(entry.stable))
            AddEntry(ahead, entry.stable, entry.age);
    }
    ahead.writtenPath = path;
    ahead.written = std::move(bytes);
    message = std::to_string(ahead.entries.size()) + " pipelines";
    return true;
}
} // namespace

void StartAhead(Device &device, const char *listPath)
{
    if (!listPath || !*listPath)
        return;
    device.ahead = new Ahead();
    Ahead &ahead = *device.ahead;
    ahead.core = device.core;
    ahead.path = listPath;
    ahead.on = Features(device.core).pipelinesAhead;
    std::string message;
    ReadList(device, listPath, message);
    device.stats.pipelinesListed = static_cast<uint32_t>(ahead.entries.size());
    Log(device, "pipeline list %s: %s; built ahead %s", listPath, message.c_str(),
        ahead.on ? "on" : "off (KISAK_VK_PIPELINES_AHEAD=0)");
}

void StopAhead(Device &device)
{
    if (!device.ahead)
        return;
    Ahead &ahead = *device.ahead;
    std::vector<PipelineKey> dropped;
    {
        std::lock_guard<std::mutex> hold(ahead.lock);
        ahead.stop = true;
        for (Job *job : ahead.queue)
            dropped.push_back(job->key);
        ahead.queue.clear();
        ahead.wake.notify_all();
    }
    for (std::thread &worker : ahead.workers)
        worker.join();
    ahead.workers.clear();
    for (const PipelineKey &key : dropped)
        ahead.jobs.erase(key);
    Collect(device);
    for (const auto &entry : ahead.modules)
    {
        if (!entry.second.kept && entry.second.module)
            DestroyShaderModule(device.core, entry.second.module);
    }
    ahead.modules.clear();
    if (device.error.empty())
    {
        std::string message;
        SavePipelineList(&device, ahead.path.c_str(), message);
        Log(device, "pipeline list %s: %s", ahead.path.c_str(), message.c_str());
    }
    delete device.ahead;
    device.ahead = nullptr;
}

void AheadShaderCreated(Device &device, Shader &shader)
{
    device.ahead->shaders[shader.contentHash].push_back(&shader);
    QueueFor(device, shader.contentHash);
}

void AheadDeclCreated(Device &device, VertexDecl &decl)
{
    device.ahead->decls[decl.contentHash].push_back(&decl);
    QueueFor(device, decl.contentHash);
}

void AheadForget(Device &device, uint32_t id, uint64_t contentHash, bool shader)
{
    Ahead &ahead = *device.ahead;
    auto drop = [id, contentHash](auto &live) {
        auto found = live.find(contentHash);
        if (found == live.end())
            return;
        auto &list = found->second;
        list.erase(std::remove_if(list.begin(), list.end(), [id](const auto *object) { return object->id == id; }),
                   list.end());
        if (list.empty())
            live.erase(found);
    };
    if (shader)
        drop(ahead.shaders);
    else
        drop(ahead.decls);
    if (!ahead.jobs.empty())
        SettleJobsFor(device, id);
    if (shader)
    {
        // Modules the workers built for this pixel shader: those it keeps go
        // with it, the rest go now.
        std::lock_guard<std::mutex> hold(ahead.lock);
        for (auto it = ahead.modules.begin(); it != ahead.modules.end();)
        {
            if (it->first.first == id && !it->second.building)
            {
                if (!it->second.kept && it->second.module)
                    DestroyShaderModule(device.core, it->second.module);
                it = ahead.modules.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

Pipeline *AheadFind(Device &device, const PipelineKey &key)
{
    Ahead &ahead = *device.ahead;
    if (ahead.jobs.empty())
        return nullptr;
    Collect(device);
    auto found = device.pipelines.find(key);
    if (found != device.pipelines.end())
        return &found->second;
    auto pending = ahead.jobs.find(key);
    if (pending == ahead.jobs.end())
        return nullptr;
    Job *job = pending->second.get();
    {
        std::unique_lock<std::mutex> hold(ahead.lock);
        if (job->state == JobState::Queued)
        {
            // Not started: the draw builds it, as it would without the list.
            for (auto it = ahead.queue.begin(); it != ahead.queue.end(); ++it)
            {
                if (*it == job)
                {
                    ahead.queue.erase(it);
                    break;
                }
            }
            hold.unlock();
            ahead.jobs.erase(pending);
            return nullptr;
        }
        const TimeInto timer(device.stats.aheadWaitMicros);
        ++device.stats.aheadWaits;
        ahead.finished.wait(hold, [job] { return job->state == JobState::Done; });
    }
    Collect(device);
    found = device.pipelines.find(key);
    return found != device.pipelines.end() ? &found->second : nullptr;
}

void AheadDrawn(Device &device, const PipelineKey &key, Pipeline &pipeline)
{
    pipeline.drawn = true;
    if (pipeline.ahead)
        ++device.stats.aheadUsed;
    if (!device.ahead)
        return;
    Ahead &ahead = *device.ahead;
    const StableKey stable = StableFor(key, pipeline.vsHash, pipeline.psHash, pipeline.declHash);
    auto found = ahead.index.find(stable);
    if (found != ahead.index.end())
        ahead.entries[found->second].drawn = true;
    else if (ahead.entries.size() < kMaxEntries)
        ahead.entries[AddEntry(ahead, stable, 0)].drawn = true;
}

bool SavePipelineList(Device *device, const char *path, std::string &message)
{
    if (!device->ahead)
    {
        message = "no list";
        return false;
    }
    Ahead &ahead = *device->ahead;
    const std::vector<uint8_t> bytes = ListBytes(*device);
    const uint32_t count = static_cast<uint32_t>((bytes.size() - sizeof(FileHeader)) / sizeof(FileEntry));
    if (ahead.writtenPath == path && bytes == ahead.written)
    {
        message = "unchanged";
        return true;
    }
    // A temporary file moved into place, so a crash never leaves half a list.
    const std::string temporary = std::string(path) + ".tmp";
    FILE *file = OpenFile(temporary.c_str(), "wb");
    if (!file)
    {
        message = "cannot write " + temporary;
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    std::remove(path); // rename does not replace an existing file on Windows
    if (!written || !closed || std::rename(temporary.c_str(), path) != 0)
    {
        std::remove(temporary.c_str());
        message = std::string("cannot write ") + path;
        return false;
    }
    ahead.writtenPath = path;
    ahead.written = bytes;
    message = std::to_string(count) + " pipelines written";
    return true;
}

bool LoadPipelineList(Device *device, const char *path, std::string &message)
{
    if (!device->ahead)
    {
        message = "no list";
        return false;
    }
    Ahead &ahead = *device->ahead;
    // Jobs name entries only through their keys: dropping the list leaves
    // them be.
    const bool ok = ReadList(*device, path, message);
    for (uint32_t at = 0; at < ahead.entries.size(); ++at)
        TryQueue(*device, at);
    return ok;
}

void FinishPipelinesAhead(Device *device)
{
    if (!device->ahead)
        return;
    Ahead &ahead = *device->ahead;
    {
        std::unique_lock<std::mutex> hold(ahead.lock);
        ahead.finished.wait(hold, [&] {
            if (!ahead.queue.empty())
                return false;
            for (const auto &entry : ahead.jobs)
            {
                if (entry.second->state != JobState::Done)
                    return false;
            }
            return true;
        });
    }
    Collect(*device);
}

uint32_t PipelineListSize(const Device *device)
{
    return device->ahead ? static_cast<uint32_t>(device->ahead->entries.size()) : 0;
}
} // namespace gfx_vk::legacy
