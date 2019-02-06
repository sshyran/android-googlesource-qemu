// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <gtest/gtest.h>

#include "GoldfishOpenglTestEnv.h"
#include "GrallocDispatch.h"
#include "GrallocUsageConversion.h"
#include "AndroidVulkanDispatch.h"

#include <vulkan/vulkan.h>
#include <vulkan/vk_android_native_buffer.h>

#include "android/base/files/PathUtils.h"
#include "android/base/memory/ScopedPtr.h"
#include "android/base/system/System.h"
#include "android/opengles.h"

#include <atomic>
#include <memory>
#include <vector>

using android::base::pj;
using android::base::System;

namespace aemu {

static constexpr int kWindowSize = 256;

static android_vulkan_dispatch* vk = nullptr;

class VulkanHalTest : public ::testing::Test {
protected:

    static GoldfishOpenglTestEnv* testEnv;

    static void SetUpTestCase() {
        testEnv = new GoldfishOpenglTestEnv;
#ifdef _WIN32
        const char* libFilename = "vulkan_android.dll";
#elif defined(__APPLE__)
        const char* libFilename = "libvulkan_android.dylib";
#else
        const char* libFilename = "libvulkan_android.so";
#endif
        auto path =
            pj(System::get()->getProgramDirectory(),
                "lib64", libFilename);
        vk = load_android_vulkan_dispatch(path.c_str());
    }

    static void TearDownTestCase() {
        // Cancel all host threads as well
        android_finishOpenglesRenderer();

        delete testEnv;
        testEnv = nullptr;

        delete vk;
    }

    void SetUp() override {
        setupGralloc();
        setupVulkan();
    }

    void TearDown() override {
        teardownVulkan();
        teardownGralloc();
    }

    void setupGralloc() {
        auto grallocPath = pj(System::get()->getProgramDirectory(), "lib64",
                              "gralloc.ranchu" LIBSUFFIX);

        load_gralloc_module(grallocPath.c_str(), &mGralloc);

        EXPECT_NE(nullptr, mGralloc.fb_dev);
        EXPECT_NE(nullptr, mGralloc.alloc_dev);
        EXPECT_NE(nullptr, mGralloc.fb_module);
        EXPECT_NE(nullptr, mGralloc.alloc_module);
    }

    void teardownGralloc() { unload_gralloc_module(&mGralloc); }

    buffer_handle_t createTestGrallocBuffer(
            int usage, int format,
            int width, int height, int* stride_out) {
        buffer_handle_t buffer;

        mGralloc.alloc(width, height, format, usage, &buffer, stride_out);
        mGralloc.registerBuffer(buffer);

        return buffer;
    }

    void destroyTestGrallocBuffer(buffer_handle_t buffer) {
        mGralloc.unregisterBuffer(buffer);
        mGralloc.free(buffer);
    }

    void setupVulkan() {
        uint32_t extCount = 0;
        std::vector<VkExtensionProperties> exts;
        EXPECT_EQ(VK_SUCCESS, vk->vkEnumerateInstanceExtensionProperties(
                                      nullptr, &extCount, nullptr));
        exts.resize(extCount);
        EXPECT_EQ(VK_SUCCESS, vk->vkEnumerateInstanceExtensionProperties(
                                      nullptr, &extCount, exts.data()));

        bool hasGetPhysicalDeviceProperties2 = false;
        bool hasExternalMemoryCapabilities = false;

        for (const auto& prop : exts) {
            if (!strcmp("VK_KHR_get_physical_device_properties2", prop.extensionName)) {
                hasGetPhysicalDeviceProperties2 = true;
            }
            if (!strcmp("VK_KHR_external_memory_capabilities", prop.extensionName)) {
                hasExternalMemoryCapabilities = true;
            }
        }

        std::vector<const char*> enabledExtensions;

        if (hasGetPhysicalDeviceProperties2) {
            enabledExtensions.push_back("VK_KHR_get_physical_device_properties2");
            mInstanceHasGetPhysicalDeviceProperties2Support = true;
        }

        if (hasExternalMemoryCapabilities) {
            enabledExtensions.push_back("VK_KHR_external_memory_capabilities");
            mInstanceHasExternalMemorySupport = true;
        }

        const char* const* enabledExtensionNames =
                enabledExtensions.size() > 0 ? enabledExtensions.data()
                                             : nullptr;

        VkInstanceCreateInfo instCi = {
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            0, 0, nullptr,
            0, nullptr,
            (uint32_t)enabledExtensions.size(),
            enabledExtensionNames,
        };

        EXPECT_EQ(VK_SUCCESS, vk->vkCreateInstance(&instCi, nullptr, &mInstance));

        uint32_t physdevCount = 0;
        std::vector<VkPhysicalDevice> physdevs;
        EXPECT_EQ(VK_SUCCESS,
                  vk->vkEnumeratePhysicalDevices(mInstance, &physdevCount, nullptr));
        physdevs.resize(physdevCount);
        EXPECT_EQ(VK_SUCCESS, vk->vkEnumeratePhysicalDevices(mInstance, &physdevCount,
                                                         physdevs.data()));

        uint32_t bestPhysicalDevice = 0;
        bool queuesGood = false;

        for (uint32_t i = 0; i < physdevCount; ++i) {
            uint32_t queueFamilyCount = 0;
            std::vector<VkQueueFamilyProperties> queueFamilyProps;
            vk->vkGetPhysicalDeviceQueueFamilyProperties(
                    physdevs[i], &queueFamilyCount, nullptr);
            queueFamilyProps.resize(queueFamilyCount);
            vk->vkGetPhysicalDeviceQueueFamilyProperties(
                    physdevs[i], &queueFamilyCount, queueFamilyProps.data());

            for (uint32_t j = 0; j < queueFamilyCount; ++j) {
                auto count = queueFamilyProps[j].queueCount;
                auto flags = queueFamilyProps[j].queueFlags;
                if (count > 0 && (flags & VK_QUEUE_GRAPHICS_BIT)) {
                    bestPhysicalDevice = i;
                    mGraphicsQueueFamily = j;
                    queuesGood = true;
                    break;
                }
            }

            if (queuesGood) {
                break;
            }
        }

        EXPECT_TRUE(queuesGood);

        mPhysicalDevice = physdevs[bestPhysicalDevice];

        VkPhysicalDeviceMemoryProperties memProps;
        vk->vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &memProps);

        bool foundHostVisibleMemoryTypeIndex = false;

        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (memProps.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                mHostVisibleMemoryTypeIndex = i;
                foundHostVisibleMemoryTypeIndex = true;
                break;
            }
        }

        EXPECT_TRUE(foundHostVisibleMemoryTypeIndex);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo dqCi = {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            0, 0,
            mGraphicsQueueFamily, 1,
            &priority,
        };

        VkDeviceCreateInfo dCi = {
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0,
            1, &dqCi,
            0, nullptr,  // no layers
            0, nullptr,  // no extensions
            nullptr,  // no features
        };

        EXPECT_EQ(VK_SUCCESS, vk->vkCreateDevice(physdevs[bestPhysicalDevice], &dCi,
                                             nullptr, &mDevice));
        vk->vkGetDeviceQueue(mDevice, mGraphicsQueueFamily, 0, &mQueue);
    }

    void teardownVulkan() {
        vk->vkDestroyDevice(mDevice, nullptr);
        vk->vkDestroyInstance(mInstance, nullptr);
    }

    void createAndroidNativeImage(buffer_handle_t* buffer_out, VkImage* image_out) {

        int usage = GRALLOC_USAGE_HW_RENDER;
        int format = HAL_PIXEL_FORMAT_RGBA_8888;
        int stride;
        buffer_handle_t buffer =
            createTestGrallocBuffer(
                usage, format, kWindowSize, kWindowSize, &stride);

        uint64_t producerUsage, consumerUsage;
        android_convertGralloc0To1Usage(usage, &producerUsage, &consumerUsage);

        VkNativeBufferANDROID nativeBufferInfo = {
            VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID, nullptr,
            buffer, stride,
            format,
            usage,
            {
                consumerUsage,
                producerUsage,
            },
        };

        VkImageCreateInfo testImageCi = {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, (const void*)&nativeBufferInfo,
            0,
            VK_IMAGE_TYPE_2D,
            VK_FORMAT_R8G8B8A8_UNORM,
            { kWindowSize, kWindowSize, 1, },
            1, 1,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            VK_SHARING_MODE_EXCLUSIVE,
            0, nullptr /* shared queue families */,
            VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VkImage testAndroidNativeImage;
        EXPECT_EQ(VK_SUCCESS, vk->vkCreateImage(mDevice, &testImageCi, nullptr,
                                            &testAndroidNativeImage));

        *buffer_out = buffer;
        *image_out = testAndroidNativeImage;
    }

    void destroyAndroidNativeImage(buffer_handle_t buffer, VkImage image) {
        vk->vkDestroyImage(mDevice, image, nullptr);
        destroyTestGrallocBuffer(buffer);
    }

    struct gralloc_implementation mGralloc;

    bool mInstanceHasGetPhysicalDeviceProperties2Support = false;
    bool mInstanceHasExternalMemorySupport = false;
    bool mDeviceHasExternalMemorySupport = false;
    bool mDeviceHasAHBSupport = false;

    VkInstance mInstance;
    VkPhysicalDevice mPhysicalDevice;
    VkDevice mDevice;
    VkQueue mQueue;
    uint32_t mHostVisibleMemoryTypeIndex;
    uint32_t mGraphicsQueueFamily;
};

// static
GoldfishOpenglTestEnv* VulkanHalTest::testEnv = nullptr;

// A basic test of Vulkan HAL:
// - Touch the Android loader at global, instance, and device level.
TEST_F(VulkanHalTest, Basic) { }

// Test: Allocate, map, flush, invalidate some host visible memory.
TEST_F(VulkanHalTest, MemoryMapping) {
    static constexpr VkDeviceSize kTestAlloc = 16 * 1024;
    VkMemoryAllocateInfo allocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0,
        kTestAlloc,
        mHostVisibleMemoryTypeIndex,
    };
    VkDeviceMemory mem;
    EXPECT_EQ(VK_SUCCESS, vk->vkAllocateMemory(mDevice, &allocInfo, nullptr, &mem));

    void* hostPtr;
    EXPECT_EQ(VK_SUCCESS, vk->vkMapMemory(mDevice, mem, 0, VK_WHOLE_SIZE, 0, &hostPtr));

    memset(hostPtr, 0xff, kTestAlloc);

    VkMappedMemoryRange toFlush = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, 0,
        mem, 0, kTestAlloc,
    };

    EXPECT_EQ(VK_SUCCESS, vk->vkFlushMappedMemoryRanges(mDevice, 1, &toFlush));
    EXPECT_EQ(VK_SUCCESS, vk->vkInvalidateMappedMemoryRanges(mDevice, 1, &toFlush));

    for (uint32_t i = 0; i < kTestAlloc; ++i) {
        EXPECT_EQ(0xff, *((uint8_t*)hostPtr + i));
    }

    int usage = GRALLOC_USAGE_HW_RENDER;
    int format = HAL_PIXEL_FORMAT_RGBA_8888;
    int stride;
    buffer_handle_t buffer =
        createTestGrallocBuffer(
            usage, format, kWindowSize, kWindowSize, &stride);

    uint64_t producerUsage, consumerUsage;
    android_convertGralloc0To1Usage(usage, &producerUsage, &consumerUsage);

    VkNativeBufferANDROID nativeBufferInfo = {
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID, nullptr,
        buffer, stride,
        format,
        usage,
        {
            consumerUsage,
            producerUsage,
        },
    };

    VkImageCreateInfo testImageCi = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, (const void*)&nativeBufferInfo,
        0,
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM,
        { kWindowSize, kWindowSize, 1, },
        1, 1,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0, nullptr /* shared queue families */,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage testAndroidNativeImage;
    EXPECT_EQ(VK_SUCCESS, vk->vkCreateImage(mDevice, &testImageCi, nullptr,
                                        &testAndroidNativeImage));
    vk->vkDestroyImage(mDevice, testAndroidNativeImage, nullptr);
    destroyTestGrallocBuffer(buffer);

    vk->vkUnmapMemory(mDevice, mem);
    vk->vkFreeMemory(mDevice, mem, nullptr);
}

// Tests creation of VkImages backed by gralloc buffers.
TEST_F(VulkanHalTest, AndroidNativeImageCreation) {
    VkImage image;
    buffer_handle_t buffer;
    createAndroidNativeImage(&buffer, &image);
    destroyAndroidNativeImage(buffer, image);
}

// Tests the path to sync Android native buffers with Gralloc buffers.
TEST_F(VulkanHalTest, AndroidNativeImageQueueSignal) {
    VkImage image;
    buffer_handle_t buffer;
    int fenceFd;

    createAndroidNativeImage(&buffer, &image);

    PFN_vkQueueSignalReleaseImageANDROID func =
        (PFN_vkQueueSignalReleaseImageANDROID)
        vk->vkGetDeviceProcAddr(mDevice, "vkQueueSignalReleaseImageANDROID");

    if (func) {
        fprintf(stderr, "%s: qsig\n", __func__);
        func(mQueue, 0, nullptr, image, &fenceFd);
    }

    destroyAndroidNativeImage(buffer, image);
}

// Tests VK_KHR_get_physical_device_properties2:
// new API: vkGetPhysicalDeviceProperties2KHR
TEST_F(VulkanHalTest, GetPhysicalDeviceProperties2) {
    if (!mInstanceHasGetPhysicalDeviceProperties2Support) {
        printf("Warning: Not testing VK_KHR_physical_device_properties2, not "
               "supported\n");
        return;
    }

    PFN_vkGetPhysicalDeviceProperties2KHR physProps2KHRFunc =
            (PFN_vkGetPhysicalDeviceProperties2KHR)vk->vkGetInstanceProcAddr(
                    mInstance, "vkGetPhysicalDeviceProperties2KHR");

    EXPECT_NE(nullptr, physProps2KHRFunc);

    VkPhysicalDeviceProperties2KHR props2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, 0,
    };

    physProps2KHRFunc(mPhysicalDevice, &props2);

    VkPhysicalDeviceProperties props;
    vk->vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);

    EXPECT_EQ(props.vendorID, props2.properties.vendorID);
    EXPECT_EQ(props.deviceID, props2.properties.deviceID);
}

// Tests VK_KHR_get_physical_device_properties2:
// new API: vkGetPhysicalDeviceFeatures2KHR
TEST_F(VulkanHalTest, GetPhysicalDeviceFeatures2KHR) {
    if (!mInstanceHasGetPhysicalDeviceProperties2Support) {
        printf("Warning: Not testing VK_KHR_physical_device_properties2, not "
               "supported\n");
        return;
    }

    PFN_vkGetPhysicalDeviceFeatures2KHR physDeviceFeatures =
            (PFN_vkGetPhysicalDeviceFeatures2KHR)vk->vkGetInstanceProcAddr(
                    mInstance, "vkGetPhysicalDeviceFeatures2KHR");

    EXPECT_NE(nullptr, physDeviceFeatures);

    VkPhysicalDeviceFeatures2 features2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, 0,
    };

    physDeviceFeatures(mPhysicalDevice, &features2);
}

// Tests VK_KHR_get_physical_device_properties2:
// new API: vkGetPhysicalDeviceImageFormatProperties2KHR
TEST_F(VulkanHalTest, GetPhysicalDeviceImageFormatProperties2KHR) {
    if (!mInstanceHasGetPhysicalDeviceProperties2Support) {
        printf("Warning: Not testing VK_KHR_physical_device_properties2, not "
               "supported\n");
        return;
    }

    PFN_vkGetPhysicalDeviceImageFormatProperties2KHR
            physDeviceImageFormatPropertiesFunc =
                    (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)
                            vk->vkGetInstanceProcAddr(mInstance,
                                                  "vkGetPhysicalDeviceImageForm"
                                                  "atProperties2KHR");

    EXPECT_NE(nullptr, physDeviceImageFormatPropertiesFunc);

    VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, 0,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT,
        0,
    };

    VkImageFormatProperties2 res = {
        VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, 0,
    };

    EXPECT_EQ(VK_SUCCESS, physDeviceImageFormatPropertiesFunc(
                                  mPhysicalDevice, &imageFormatInfo, &res));
}

// Tests that if we create an instance and the API version is less than 1.1,
// we return null for 1.1 core API calls.
TEST_F(VulkanHalTest, Hide1_1FunctionPointers) {
    VkPhysicalDeviceProperties props;

    vk->vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);

    if (props.apiVersion < VK_API_VERSION_1_1) {
        EXPECT_EQ(nullptr,
                  vk->vkGetDeviceProcAddr(mDevice, "vkTrimCommandPool"));
    } else {
        EXPECT_NE(nullptr,
                  vk->vkGetDeviceProcAddr(mDevice, "vkTrimCommandPool"));
    }
}

}  // namespace aemu