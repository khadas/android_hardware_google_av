/*
 * Copyright 2018, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef STAGEFRIGHT_C2_PLATFORM_STORE_PLUGIN_LOADER_H_

#define STAGEFRIGHT_C2_PLATFORM_STORE_PLUGIN_LOADER_H_

#include <memory>

#include <utils/Mutex.h>

#include <C2.h>
#include <C2Buffer.h>

/**
 * Extern C interface for creating block pool from platform store extension.
 *
 * \param alloctorId  the ID of the backing allocator type.
 * \parem blockPoolId the ID of created block pool.
 *
 * \return pointer of created C2BlockPool, nullptr on error.
 */
typedef ::C2BlockPool* (*CreateBlockPoolFunc)(::C2Allocator::id_t, ::C2BlockPool::local_id_t);

class C2PlatformStorePluginLoader {
public:
    static const std::unique_ptr<C2PlatformStorePluginLoader>& GetInstance();
    ~C2PlatformStorePluginLoader();

    /**
     * Creates block pool from platform store extension.
     *
     * \param alloctorId  the ID of the backing allocator type.
     * \param blockPoolId the ID of created block pool.
     * \param pool        shared pointer where the created block pool is stored.
     *
     * \retval C2_OK        the block pool was created successfully.
     * \retval C2_NOT_FOUND the extension symbol was not found.
     * \retval C2_BAD_INDEX the input allocatorId is not defined in platform store extension.
     */
    c2_status_t createBlockPool(::C2Allocator::id_t allocatorId,
                                ::C2BlockPool::local_id_t blockPoolId,
                                std::shared_ptr<C2BlockPool>* pool);

private:
    explicit C2PlatformStorePluginLoader(const char *libPath);

    static android::Mutex sMutex;
    static std::unique_ptr<C2PlatformStorePluginLoader> sInstance;

    void *mLibHandle;
    CreateBlockPoolFunc mCreateBlockPool;
};

#endif  // STAGEFRIGHT_C2_PLATFORM_STORE_PLUGIN_LOADER_H_
