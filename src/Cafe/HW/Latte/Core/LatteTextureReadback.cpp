#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cemu/Telemetry/Telemetry.h"
#include <unordered_map>

#define LOG_READBACK_TIME

struct LatteTextureReadbackQueueEntry
{
	HRTick initiateTime;
	uint32 lastUpdateDrawcallIndex;
	LatteTextureView* textureView;
};

// physAddr -> how many times a readback has been queued for it. The detail string is the
// dedup key, so the varying count has to live outside it.
std::unordered_map<uint32, uint64> sReadbackQueueCount;

std::vector<LatteTextureReadbackQueueEntry> sTextureScheduledReadbacks; // readbacks that have been queued but the actual transfer has not yet been started
std::queue<LatteTextureReadbackInfo*> sTextureActiveReadbackQueue; // readbacks in flight

void LatteTextureReadback_StartTransfer(LatteTextureView* textureView)
{
	cemuLog_log(LogType::TextureReadback, "[TextureReadback-Start] PhysAddr {:08x} Res {}x{} Fmt {} Slice {} Mip {}", textureView->baseTexture->physAddress, textureView->baseTexture->width, textureView->baseTexture->height, textureView->baseTexture->format, textureView->firstSlice, textureView->firstMip);
	HRTick currentTick = HighResolutionTimer().now().getTick();
	// create info entry and store in ordered linked list
	LatteTextureReadbackInfo* readbackInfo = g_renderer->texture_createReadback(textureView);
	if (!readbackInfo)
		return;
	sTextureActiveReadbackQueue.push(readbackInfo);
	readbackInfo->StartTransfer();
	readbackInfo->transferStartTime = currentTick;
}

/*
 * Checks for queued transfers and starts them if at least five drawcalls have passed since the last write
 * Called after a draw sequence is completed
 * Returns true if at least one transfer was started
 */
bool LatteTextureReadback_Update(bool forceStart)
{
	bool hasStartedTransfer = false;
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		uint32 numElapsedDrawcalls = LatteGPUState.drawCallCounter - entry.lastUpdateDrawcallIndex;
		if (forceStart || numElapsedDrawcalls >= 5)
		{
#ifdef LOG_READBACK_TIME
			double elapsedSecondsSinceInitiate = HighResolutionTimer::getTimeDiff(entry.initiateTime, HighResolutionTimer().now().getTick());
			cemuLog_log(LogType::TextureReadback, "[TextureReadback-Update] Starting transfer for {:08x} after {} elapsed drawcalls. Time since initiate: {:.4} Force-start: {}", entry.textureView->baseTexture->physAddress, numElapsedDrawcalls, elapsedSecondsSinceInitiate, forceStart?"yes":"no");
#endif
			LatteTextureReadback_StartTransfer(entry.textureView);
			// remove element
			vectorRemoveByIndex(sTextureScheduledReadbacks, i);
			i--;
			hasStartedTransfer = true;
		}
	}
	return hasStartedTransfer;
}

/*
 * Called when a texture is deleted
 */
void LatteTextureReadback_NotifyTextureDeletion(LatteTexture* texture)
{
	// delete from queue
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		if (entry.textureView->baseTexture == texture)
		{
			vectorRemoveByIndex(sTextureScheduledReadbacks, i);
			break;
		}
	}
}

void LatteTextureReadback_Initate(LatteTextureView* textureView)
{
	// currently we don't support readback for resized textures
	if (textureView->baseTexture->overwriteInfo.hasResolutionOverwrite)
	{
		cemuLog_log(LogType::Force, "Texture readback is not supported for textures with modified resolution. Texture: {:08x} {}x{}", textureView->baseTexture->physAddress, textureView->baseTexture->width, textureView->baseTexture->height);
		return;
	}
	// check if texture isn't already queued for transfer
	for (size_t i = 0; i < sTextureScheduledReadbacks.size(); i++)
	{
		LatteTextureReadbackQueueEntry& entry = sTextureScheduledReadbacks[i];
		if (entry.textureView == textureView)
		{
			entry.lastUpdateDrawcallIndex = LatteGPUState.drawCallCounter;
			return;
		}
	}
	// Identify what is actually being mirrored to CPU RAM. enableReadback is set for ANY
	// TM_LINEAR_ALIGNED texture (LatteTexture.cpp:1311) on the theory that the CPU can read a
	// linear surface directly -- but the emulator cannot know whether the guest ever will, so
	// it mirrors defensively. That defence costs a full GPU pipeline drain at every
	// GX2DrawDone: 6.75 ms/frame in BotW, which is the entire 20-vs-30 fps gap. If the target
	// turns out to be something the guest never reads, the whole cost is for nothing.
	// Frame-vector counter (the half NoteAccuracyDetail does not touch).
	TLM_INC(Accuracy, AccReadbackQueued);
	if (tlm::AreaEnabled(tlm::Area::Accuracy)) [[unlikely]]
	{
		LatteTexture* t = textureView->baseTexture;
		tlm::NoteAccuracyDetail(tlm::CounterId::AccReadbackQueued,
			fmt::format("{:08x} {}x{} fmt {:04x} tm {} mips {} depth {}",
						t->physAddress, t->width, t->height, (uint32)t->format,
						(uint32)t->tileMode, t->mipLevels, t->isDepth ? 1 : 0),
			++sReadbackQueueCount[t->physAddress]);
	}

	// queue
	LatteTextureReadbackQueueEntry queueEntry;
	queueEntry.initiateTime = HighResolutionTimer().now().getTick();
	queueEntry.textureView = textureView;
	queueEntry.lastUpdateDrawcallIndex = LatteGPUState.drawCallCounter;
	sTextureScheduledReadbacks.emplace_back(queueEntry);
}

void LatteTextureReadback_UpdateFinishedTransfers(bool forceFinish)
{
	if (forceFinish)
	{
		// Start any delayed transfers. Note what this means: a transfer force-started HERE has
		// had zero time on the GPU before the loop below blocks on it, which is the worst
		// possible arrangement. Counted separately from the finish, because "the 5-drawcall
		// delay held it back" and "it has been in flight and the GPU is behind" need different
		// fixes and cost the same on the clock.
		if (LatteTextureReadback_Update(true))
			TLM_INC(Gpu, GpuReadbackForceStart);
	}
	performanceMonitor.gpuTime_waitForAsync.beginMeasuring();
	while (!sTextureActiveReadbackQueue.empty())
	{
		LatteTextureReadbackInfo* readbackInfo = sTextureActiveReadbackQueue.front();
		if (forceFinish)
		{
			if (!readbackInfo->IsFinished())
			{
				readbackInfo->waitStartTime = HighResolutionTimer().now().getTick();
#ifdef LOG_READBACK_TIME
				if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
				{
					double elapsedSecondsTransfer = HighResolutionTimer::getTimeDiff(readbackInfo->transferStartTime, HighResolutionTimer().now().getTick());
					cemuLog_log(LogType::TextureReadback, "[Texture-Readback] Force-finish: {:08x} Res {:}/{:} TM {:} FMT {:04x} Transfer time so far: {:.4}ms", readbackInfo->hostTextureCopy.physAddress, readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.tileMode, (uint32)readbackInfo->hostTextureCopy.format, elapsedSecondsTransfer * 1000.0);
				}
#endif
				readbackInfo->forceFinish = true;
				// How long this transfer actually got to run before we blocked on it. Near zero
				// means it was force-started moments ago; milliseconds means it was in flight
				// and the GPU is what we are really waiting for.
				TLM_ADD(Gpu, GpuReadbackAgeAtWaitNs,
						HighResolutionTimer().now().getTick() - readbackInfo->transferStartTime);
				readbackInfo->ForceFinish();
				// rerun logic since ->ForceFinish() can recurively call this function and thus modify the queue
				continue;
			}
		}
		else
		{
			if (!readbackInfo->IsFinished())
				break;
			readbackInfo->waitStartTime = HighResolutionTimer().now().getTick();
		}
		// performance testing
#ifdef LOG_READBACK_TIME
		if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
		{
			HRTick currentTick = HighResolutionTimer().now().getTick();
			double elapsedSecondsTransfer = HighResolutionTimer::getTimeDiff(readbackInfo->transferStartTime, currentTick);
			double elapsedSecondsWaiting = HighResolutionTimer::getTimeDiff(readbackInfo->waitStartTime, currentTick);
			cemuLog_log(LogType::TextureReadback, "[Texture-Readback] {:08x} Res {}/{} TM {} FMT {:04x} ReadbackLatency: {:6.3}ms WaitTime: {:6.3}ms ForcedWait {}", readbackInfo->hostTextureCopy.physAddress, readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.tileMode, (uint32)readbackInfo->hostTextureCopy.format, elapsedSecondsTransfer * 1000.0, elapsedSecondsWaiting * 1000.0, readbackInfo->forceFinish ? "yes" : "no");
		}
#endif
		uint8* pixelData = readbackInfo->GetData();
		LatteTextureLoader_writeReadbackTextureToMemory(&readbackInfo->hostTextureCopy, 0, 0, pixelData);
		readbackInfo->ReleaseData();
		// get the original texture if it still exists and invalidate the current data hash
		LatteTextureView* origTexView = LatteTextureViewLookupCache::lookupSlice(readbackInfo->hostTextureCopy.physAddress, readbackInfo->hostTextureCopy.width, readbackInfo->hostTextureCopy.height, readbackInfo->hostTextureCopy.pitch, 0, 0, readbackInfo->hostTextureCopy.format);
		if (origTexView)
			LatteTC_ResetTextureChangeTracker(origTexView->baseTexture, true);
		delete readbackInfo;
		// remove from queue
		cemu_assert_debug(!sTextureActiveReadbackQueue.empty());
		cemu_assert_debug(readbackInfo == sTextureActiveReadbackQueue.front());
		sTextureActiveReadbackQueue.pop();
	}
	performanceMonitor.gpuTime_waitForAsync.endMeasuring();
}

bool LatteTextureReadback_ReadbackToLinearBlocking(LatteTextureView* sourceView, uint8* dstPtr, uint32 dstWidth, uint32 dstHeight, uint32 dstPitch)
{
	LatteTextureReadbackInfo* info = g_renderer->texture_createReadback(sourceView);
	if (!info)
		return false;

	info->StartTransfer();
	info->ForceFinish();
	cemu_assert(info->IsFinished());

	uint8* data = info->GetData(); // returned pixel format should match Latte format
	uint32 bpp = Latte::GetFormatBits(sourceView->baseTexture->format) / 8;
	uint32 srcRowBytes = sourceView->baseTexture->width * bpp;
	uint32 dstRowBytes = dstWidth * bpp;
	for (uint32 y = 0; y < dstHeight; y++)
		memcpy(dstPtr + y * dstPitch * bpp, data + y * srcRowBytes, dstRowBytes);

	info->ReleaseData();
	delete info;
	return true;
}