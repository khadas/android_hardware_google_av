/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "C2SoftFlacDec"
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include "C2SoftFlacDec.h"

namespace android {

constexpr char COMPONENT_NAME[] = "c2.android.flac.decoder";

class C2SoftFlacDec::IntfImpl : public C2InterfaceHelper {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : C2InterfaceHelper(helper) {

        setDerivedInstance(this);

        addParameter(
                DefineParam(mInputFormat, C2_NAME_INPUT_STREAM_FORMAT_SETTING)
                .withConstValue(new C2StreamFormatConfig::input(0u, C2FormatCompressed))
                .build());

        addParameter(
                DefineParam(mOutputFormat, C2_NAME_OUTPUT_STREAM_FORMAT_SETTING)
                .withConstValue(new C2StreamFormatConfig::output(0u, C2FormatAudio))
                .build());

        addParameter(
                DefineParam(mInputMediaType, C2_NAME_INPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::input>(
                        MEDIA_MIMETYPE_AUDIO_FLAC))
                .build());

        addParameter(
                DefineParam(mOutputMediaType, C2_NAME_OUTPUT_PORT_MIME_SETTING)
                .withConstValue(AllocSharedString<C2PortMimeConfig::output>(
                        MEDIA_MIMETYPE_AUDIO_RAW))
                .build());

        addParameter(
                DefineParam(mSampleRate, C2_NAME_STREAM_SAMPLE_RATE_SETTING)
                .withDefault(new C2StreamSampleRateInfo::output(0u, 44100))
                .withFields({C2F(mSampleRate, value).inRange(1, 655350)})
                .withSetter((Setter<decltype(*mSampleRate)>::StrictValueWithNoDeps))
                .build());

        addParameter(
                DefineParam(mChannelCount, C2_NAME_STREAM_CHANNEL_COUNT_SETTING)
                .withDefault(new C2StreamChannelCountInfo::output(0u, 1))
                .withFields({C2F(mChannelCount, value).inRange(1, 8)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_NAME_STREAM_BITRATE_SETTING)
                .withDefault(new C2BitrateTuning::input(0u, 768000))
                .withFields({C2F(mBitrate, value).inRange(1, 21000000)})
                .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
                .build());
    }

private:
    std::shared_ptr<C2StreamFormatConfig::input> mInputFormat;
    std::shared_ptr<C2StreamFormatConfig::output> mOutputFormat;
    std::shared_ptr<C2PortMimeConfig::input> mInputMediaType;
    std::shared_ptr<C2PortMimeConfig::output> mOutputMediaType;
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2BitrateTuning::input> mBitrate;
};

C2SoftFlacDec::C2SoftFlacDec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mFLACDecoder(nullptr) {
}

C2SoftFlacDec::~C2SoftFlacDec() {
    delete mFLACDecoder;
}

c2_status_t C2SoftFlacDec::onInit() {
    status_t err = initDecoder();
    return err == OK ? C2_OK : C2_NO_MEMORY;
}

c2_status_t C2SoftFlacDec::onStop() {
    if (mFLACDecoder) mFLACDecoder->flush();
    memset(&mStreamInfo, 0, sizeof(mStreamInfo));
    mHasStreamInfo = false;
    mSignalledError = false;
    mSignalledOutputEos = false;
    mInputBufferCount = 0;
    return C2_OK;
}

void C2SoftFlacDec::onReset() {
    (void)onStop();
}

void C2SoftFlacDec::onRelease() {
}

c2_status_t C2SoftFlacDec::onFlush_sm() {
    return onStop();
}

status_t C2SoftFlacDec::initDecoder() {
    if (mFLACDecoder) {
        delete mFLACDecoder;
    }
    mFLACDecoder = FLACDecoder::Create();
    if (!mFLACDecoder) {
        ALOGE("initDecoder: failed to create FLACDecoder");
        mSignalledError = true;
        return NO_MEMORY;
    }

    memset(&mStreamInfo, 0, sizeof(mStreamInfo));
    mHasStreamInfo = false;
    mSignalledError = false;
    mSignalledOutputEos = false;
    mInputBufferCount = 0;

    return OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

// (TODO) add multiframe support, in plugin and FLACDecoder.cpp
void C2SoftFlacDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.configUpdate.clear();
    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    C2ReadView rView = mDummyReadView;
    size_t inOffset = 0u;
    size_t inSize = 0u;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            ALOGE("read view map failed %d", rView.error());
            work->result = C2_CORRUPTED;
            return;
        }
    }
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    bool codecConfig = (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) != 0;

    ALOGV("in buffer attr. size %zu timestamp %d frameindex %d", inSize,
          (int)work->input.ordinal.timestamp.peeku(), (int)work->input.ordinal.frameIndex.peeku());

    if (inSize == 0) {
        fillEmptyWork(work);
        if (eos) {
            mSignalledOutputEos = true;
            ALOGV("signalled EOS");
        }
        return;
    }

    if (mInputBufferCount == 0 && !codecConfig) {
        ALOGV("First frame has to include configuration, forcing config");
        codecConfig = true;
    }

    uint8_t *input = const_cast<uint8_t *>(rView.data() + inOffset);
    if (codecConfig) {
        status_t decoderErr = mFLACDecoder->parseMetadata(input, inSize);
        if (decoderErr != OK && decoderErr != WOULD_BLOCK) {
            ALOGE("process: FLACDecoder parseMetaData returns error %d", decoderErr);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }

        mInputBufferCount++;
        fillEmptyWork(work);
        if (eos) {
            mSignalledOutputEos = true;
            ALOGV("signalled EOS");
        }

        if (decoderErr == WOULD_BLOCK) {
            ALOGV("process: parseMetadata is Blocking, Continue %d", decoderErr);
        } else {
            mStreamInfo = mFLACDecoder->getStreamInfo();
            if (mStreamInfo.sample_rate && mStreamInfo.max_blocksize &&
                mStreamInfo.channels) {
                mHasStreamInfo = true;
                C2StreamSampleRateInfo::output sampleRateInfo(
                    0u, mStreamInfo.sample_rate);
                C2StreamChannelCountInfo::output channelCountInfo(
                    0u, mStreamInfo.channels);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                c2_status_t err =
                    mIntf->config({&sampleRateInfo, &channelCountInfo},
                                  C2_MAY_BLOCK, &failures);
                if (err == OK) {
                    work->worklets.front()->output.configUpdate.push_back(
                        C2Param::Copy(sampleRateInfo));
                    work->worklets.front()->output.configUpdate.push_back(
                        C2Param::Copy(channelCountInfo));
                } else {
                    ALOGE("Config Update failed");
                    mSignalledError = true;
                    work->result = C2_CORRUPTED;
                    return;
                }
            }
            ALOGD("process: decoder configuration : %d Hz, %d channels, %d samples,"
                  " %d block size", mStreamInfo.sample_rate, mStreamInfo.channels,
                  (int)mStreamInfo.total_samples, mStreamInfo.max_blocksize);
        }
        return;
    }

    size_t outSize;
    if (mHasStreamInfo)
        outSize = mStreamInfo.max_blocksize * mStreamInfo.channels * sizeof(short);
    else
        outSize = kMaxBlockSize * FLACDecoder::kMaxChannels * sizeof(short);

    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    c2_status_t err = pool->fetchLinearBlock(outSize, usage, &block);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock for Output failed with status %d", err);
        work->result = C2_NO_MEMORY;
        return;
    }
    C2WriteView wView = block->map().get();
    if (wView.error()) {
        ALOGE("write view map failed %d", wView.error());
        work->result = C2_CORRUPTED;
        return;
    }

    short *output = reinterpret_cast<short *>(wView.data());
    status_t decoderErr = mFLACDecoder->decodeOneFrame(
                            input, inSize, output, &outSize);
    if (decoderErr != OK) {
        ALOGE("process: FLACDecoder decodeOneFrame returns error %d", decoderErr);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        return;
    }

    mInputBufferCount++;
    ALOGV("out buffer attr. size %zu", outSize);
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(createLinearBuffer(block, 0, outSize));
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
    if (eos) {
        mSignalledOutputEos = true;
        ALOGV("signalled EOS");
    }
}

c2_status_t C2SoftFlacDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void) pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    if (mFLACDecoder) mFLACDecoder->flush();

    return C2_OK;
}

class C2SoftFlacDecFactory : public C2ComponentFactory {
public:
    C2SoftFlacDecFactory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2PlatformComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2SoftFlacDec(COMPONENT_NAME,
                                      id,
                                      std::make_shared<C2SoftFlacDec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2SoftFlacDec::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2SoftFlacDec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual ~C2SoftFlacDecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftFlacDecFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
