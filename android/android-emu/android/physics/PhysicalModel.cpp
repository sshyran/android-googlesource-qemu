/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "android/physics/PhysicalModel.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/vec3.hpp>

#include "android/automation/AutomationController.h"
#include "android/automation/AutomationEventSink.h"
#include "android/base/async/ThreadLooper.h"
#include "android/base/files/PathUtils.h"
#include "android/base/files/StdioStream.h"
#include "android/base/system/System.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/hw-sensors.h"
#include "android/physics/AmbientEnvironment.h"
#include "android/physics/InertialModel.h"
#include "android/utils/stream.h"

#include <cstdio>
#include <mutex>

#define D(...) VERBOSE_PRINT(sensors, __VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define E(...) derror(__VA_ARGS__)

using android::automation::AutomationController;
using android::base::PathUtils;
using android::base::StdioStream;
using android::base::System;

namespace pb = emulator_automation;

namespace android {
namespace physics {

/*
 * Implements a model of an ambient environment containing a rigid
 * body, and produces accurately simulated sensor values for various
 * sensors in this environment.
 *
 * The physical model should be updated with target ambient and rigid
 * body state, and regularly polled for the most recent sensor values.
 *
 * Components that only require updates when the model is actively
 * changing (i.e. not at rest) should register state change callbacks
 * via setStateChangingCallbacks.  TargetStateChange callbacks occur
 * on the same thread that setTargetXXXXXX is called from.  Sensor
 * state changing callbacks may occur on an arbitrary thread.
 */
class PhysicalModelImpl {
public:
    PhysicalModelImpl();
    ~PhysicalModelImpl();

    /*
     * Gets the PhysicalModel interface for this object.
     * Can be called from any thread.
     */
    PhysicalModel* getPhysicalModel();

    static PhysicalModelImpl* getImpl(PhysicalModel* physicalModel);

    /*
     * Sets the current time of the PhysicalModel simulation.  This time is
     * used as the current time in calculating all sensor values, along with
     * the time when target parameter change requests are recorded as taking
     * place.  Time values must be non-decreasing.
     *
     * Returns whether the physical state is stable or changing at the set time.
     */
    void setCurrentTime(int64_t time_ns);

    /*
     * Replays a PhysicalModelEvent onto the current PhysicalModel state.
     */
    void replayEvent(const pb::PhysicalModelEvent& event);

    /*
     * Sets the target value for the given physical parameter that the physical
     * model should move towards and records the result.
     * Can be called from any thread.
     */
#define SET_TARGET_FUNCTION_NAME(x) setTarget##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
    void SET_TARGET_FUNCTION_NAME(z)(\
        w value, PhysicalInterpolation mode);

    PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef SET_TARGET_FUNCTION_NAME

    /*
     * Gets current target state of the modeled object.
     * Can be called from any thread.
     */
#define GET_TARGET_FUNCTION_NAME(x) getParameter##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
    w GET_TARGET_FUNCTION_NAME(z)(ParameterValueType parameterValueType) const;

    PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef GET_TARGET_FUNCTION_NAME

    /*
     * Sets the current override states for each simulated sensor.
     * Can be called from any thread.
     */
#define OVERRIDE_FUNCTION_NAME(x) override##x
#define SENSOR_(x,y,z,v,w) \
    void OVERRIDE_FUNCTION_NAME(z)(v override_value);

    SENSORS_LIST
#undef SENSOR_
#undef OVERRIDE_FUNCTION_NAME

    /*
     * Getters for all sensor values.
     * Can be called from any thread.
     */
#define GET_FUNCTION_NAME(x) get##x
#define SENSOR_(x,y,z,v,w) \
    v GET_FUNCTION_NAME(z)(long* measurement_id) const;

    SENSORS_LIST
#undef SENSOR_
#undef GET_FUNCTION_NAME

    /* Get the physical rotation and translation simulatenousely. */
    void getTransform(
            float* out_translation_x, float* out_translation_y, float* out_translation_z,
            float* out_rotation_x, float* out_rotation_y, float* out_rotation_z,
            int64_t* out_timestamp);

    /*
     * Set or unset callbacks used to signal changing state.
     * Can be called from any thread.
     */
    void setPhysicalStateAgent(const QAndroidPhysicalStateAgent* agent);

    /*
     * Set or unset the automation controller.
     * Can be called from any thread.
     */
    void setAutomationController(AutomationController* controller);

    /*
     * Save the full physical state to the given stream for snapshots.
     */
    void snapshotSave(Stream* f);

    /*
     * Load the full physical state from the given stream.
     */
    int snapshotLoad(Stream* f);

    /*
     * Save physical model state, used for automation.  Does not include
     * overrides.
     */
    void saveState(pb::InitialState* state);

    /*
     * Load physical model state, used for automation.
     */
    void loadState(const pb::InitialState& state);

    /*
     * Start recording physical model ground truth to the given file.
     * Returns 0 if successful.
     */
    int recordGroundTruth(const char* filename);

    /*
     * Stop recording the ground truth.
     */
    void stopRecordGroundTruth();

private:
    /*
     * Sets the target value for the given physical parameter that the physical
     * model should move towards.
     * Can be called from any thread.
     */
#define SET_TARGET_INTERNAL_FUNCTION_NAME(x) setTargetInternal##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
    void SET_TARGET_INTERNAL_FUNCTION_NAME(z)(w value, \
                                              PhysicalInterpolation mode);

    PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef SET_TARGET_INTERNAL_FUNCTION_NAME

    /*
     * Getters for non-overridden physical sensor values.
     */
#define GET_PHYSICAL_NAME(x) getPhysical##x
#define SENSOR_(x,y,z,v,w) \
    v GET_PHYSICAL_NAME(z)() const;

    SENSORS_LIST
#undef SENSOR_
#undef GET_PHYSICAL_NAME

    /*
     * Helper for setting overrides.
     */
    template <class T>
    void setOverride(AndroidSensor sensor,
                     T* overrideMemberPointer,
                     T overrideValue) {
        physicalStateChanging();
        {
            std::lock_guard<std::recursive_mutex> lock(mMutex);
            mUseOverride[sensor] = true;
            mMeasurementId[sensor]++;
            *overrideMemberPointer = overrideValue;
        }
    }

    /*
     * Helper for getting current sensor values.
     */
    template<class T>
    T getSensorValue(AndroidSensor sensor,
                     const T* overrideMemberPointer,
                     std::function<T()> physicalGetter,
                     long* measurement_id) const {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        if (mUseOverride[sensor]) {
            *measurement_id = mMeasurementId[sensor];
            return *overrideMemberPointer;
        } else {
            if (mIsPhysicalStateChanging) {
                mMeasurementId[sensor]++;
            }
            *measurement_id = mMeasurementId[sensor];
            return physicalGetter();
        }
    }

    void physicalStateChanging();
    void physicalStateStabilized();
    void targetStateChanged();

    template <typename ValueType>
    void generateEvent(PhysicalParameter type,
                       PhysicalInterpolation mode,
                       ValueType value);

    mutable std::recursive_mutex mMutex;

    InertialModel mInertialModel;
    AmbientEnvironment mAmbientEnvironment;

    AutomationController* mAutomationController = nullptr;
    const QAndroidPhysicalStateAgent* mAgent = nullptr;
    bool mIsPhysicalStateChanging = false;

    bool mUseOverride[MAX_SENSORS] = {false};
    mutable long mMeasurementId[MAX_SENSORS] = {0};
#define OVERRIDE_NAME(x) m##x##Override
#define SENSOR_(x,y,z,v,w) \
    v OVERRIDE_NAME(z){0.f};

    SENSORS_LIST
#undef SENSOR_
#undef OVERRIDE_NAME

    std::unique_ptr<StdioStream> mGroundTruthStream;

    int64_t mModelTimeNs = 0L;

    PhysicalModel mPhysicalModelInterface;
};

static glm::vec3 toGlm(vec3 input) {
    return glm::vec3(input.x, input.y, input.z);
}

static vec3 fromGlm(glm::vec3 input) {
    vec3 value;
    value.x = input.x;
    value.y = input.y;
    value.z = input.z;
    return value;
}

PhysicalModelImpl::PhysicalModelImpl() {
    mPhysicalModelInterface.opaque = reinterpret_cast<void*>(this);
}

PhysicalModelImpl::~PhysicalModelImpl() {
    assert(mAgent == nullptr);
}

PhysicalModel* PhysicalModelImpl::getPhysicalModel() {
    return &mPhysicalModelInterface;
}

PhysicalModelImpl* PhysicalModelImpl::getImpl(PhysicalModel* physicalModel) {
    return physicalModel != nullptr ?
            reinterpret_cast<PhysicalModelImpl*>(physicalModel->opaque) :
            nullptr;
}

pb::PhysicalModelEvent_ParameterType toProto(PhysicalParameter param) {
    switch (param) {
#define PHYSICAL_PARAMETER_ENUM(x) PHYSICAL_PARAMETER_##x
#define PROTO_ENUM(x) pb::PhysicalModelEvent_ParameterType_##x

#define PHYSICAL_PARAMETER_(x, y, z, w) \
    case PHYSICAL_PARAMETER_ENUM(x):    \
        return PROTO_ENUM(x);

    PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef PHYSICAL_PARAMETER_ENUM
        default:
            assert(false);  // should never happen
            return pb::PhysicalModelEvent_ParameterType_ParameterType_MIN;
    }
}

PhysicalParameter fromProto(pb::PhysicalModelEvent_ParameterType param) {
    switch (param) {
#define PHYSICAL_PARAMETER_ENUM(x) PHYSICAL_PARAMETER_##x
#define PROTO_ENUM(x) pb::PhysicalModelEvent_ParameterType_##x

#define PHYSICAL_PARAMETER_(x,y,z,w) \
        case PROTO_ENUM(x):\
            return PHYSICAL_PARAMETER_ENUM(x);

        PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef PHYSICAL_PARAMETER_ENUM
        default:
            W("%s: Unknown physical parameter %d.", __FUNCTION__, param);
            return MAX_PHYSICAL_PARAMETERS;
    }
}

template <typename T>
T getProtoValue(const pb::PhysicalModelEvent_ParameterValue& parameter);

template <>
float getProtoValue<float>(const pb::PhysicalModelEvent_ParameterValue& parameter) {
    if (parameter.data_size() != 1) {
        W("%s: Error in parsed physics command.  Float parameters should have "
          "exactly one value.  Found %d.",
          __FUNCTION__, parameter.data_size());
        return 0.f;
    }
    return parameter.data(0);
}

template <>
vec3 getProtoValue<vec3>(const pb::PhysicalModelEvent_ParameterValue& parameter) {
    if (parameter.data_size() != 3) {
        W("%s: Error in parsed physics command.  Vec3 parameters should have "
          "exactly three values.  Found %d.",
          __FUNCTION__, parameter.data_size());
        return vec3{0.f, 0.f, 0.f};
    }
    return vec3{parameter.data(0), parameter.data(1), parameter.data(2)};
}

void setProtoCurrentValue(pb::PhysicalModelEvent* event, float value) {
    event->mutable_current_value()->add_data(value);
}

void setProtoCurrentValue(pb::PhysicalModelEvent* event, vec3 value) {
    pb::PhysicalModelEvent_ParameterValue* pbValue =
            event->mutable_current_value();
    pbValue->add_data(value.x);
    pbValue->add_data(value.y);
    pbValue->add_data(value.z);
}

void setProtoTargetValue(pb::PhysicalModelEvent* event, float value) {
    event->mutable_target_value()->add_data(value);
}

void setProtoTargetValue(pb::PhysicalModelEvent* event, vec3 value) {
    pb::PhysicalModelEvent_ParameterValue* pbValue =
            event->mutable_target_value();
    pbValue->add_data(value.x);
    pbValue->add_data(value.y);
    pbValue->add_data(value.z);
}

void PhysicalModelImpl::setCurrentTime(int64_t time_ns) {
    bool stateStabilized = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mModelTimeNs = time_ns;
        const bool isInertialModelStable =
                mInertialModel.setCurrentTime(time_ns) == INERTIAL_STATE_STABLE;
        const bool isAmbientModelStable =
                mAmbientEnvironment.setCurrentTime(time_ns) ==
                AMBIENT_STATE_STABLE;
        stateStabilized = (isInertialModelStable && isAmbientModelStable &&
                           mIsPhysicalStateChanging);
    }

    if (stateStabilized) {
        physicalStateStabilized();
    }
}

void PhysicalModelImpl::replayEvent(const pb::PhysicalModelEvent& event) {
    switch (fromProto(event.type())) {
#define PHYSICAL_PARAMETER_ENUM(x) PHYSICAL_PARAMETER_##x
#define SET_TARGET_INTERNAL_FUNCTION_NAME(x) setTargetInternal##x
#define GET_PROTO_VALUE_FUNCTION_NAME(x) getProtoValue_##x
#define PHYSICAL_PARAMETER_(x, y, z, type)                  \
    case PHYSICAL_PARAMETER_ENUM(x):                        \
        if (event.has_current_value()) {                    \
            SET_TARGET_INTERNAL_FUNCTION_NAME(z)(           \
                getProtoValue<type>(event.current_value()), \
                PHYSICAL_INTERPOLATION_STEP);               \
        }                                                   \
        if (event.has_target_value()) {                     \
            SET_TARGET_INTERNAL_FUNCTION_NAME(z)(           \
                getProtoValue<type>(event.target_value()),  \
                PHYSICAL_INTERPOLATION_SMOOTH);             \
        }                                                   \
        break;

        PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef GET_PROTO_VALUE_FUNCTION_NAME
#undef SET_TARGET_INTERNAL_FUNCTION_NAME
#undef PHYSICAL_PARAMETER_ENUM
        default:
            break;
    }
}

void PhysicalModelImpl::setTargetInternalPosition(vec3 position,
        PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mInertialModel.setTargetPosition(toGlm(position), mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalVelocity(vec3 velocity,
        PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mInertialModel.setTargetVelocity(toGlm(velocity), mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalAmbientMotion(float bounds,
        PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mInertialModel.setTargetAmbientMotion(bounds, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalRotation(vec3 rotation,
        PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mInertialModel.setTargetRotation(
                glm::toQuat(glm::eulerAngleXYZ(glm::radians(rotation.x),
                                               glm::radians(rotation.y),
                                               glm::radians(rotation.z))),
                mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalMagneticField(
        vec3 field,
        PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setMagneticField(field.x, field.y, field.z, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalTemperature(
        float celsius, PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setTemperature(celsius, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalProximity(
        float centimeters, PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setProximity(centimeters, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalLight(
        float lux, PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setLight(lux, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalPressure(
        float hPa, PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setPressure(hPa, mode);
    }
    targetStateChanged();
}

void PhysicalModelImpl::setTargetInternalHumidity(
        float percentage, PhysicalInterpolation mode) {
    physicalStateChanging();
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAmbientEnvironment.setHumidity(percentage, mode);
    }
    targetStateChanged();
}

vec3 PhysicalModelImpl::getParameterPosition(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return fromGlm(mInertialModel.getPosition(parameterValueType));
}

vec3 PhysicalModelImpl::getParameterVelocity(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return fromGlm(mInertialModel.getVelocity(parameterValueType));
}

float PhysicalModelImpl::getParameterAmbientMotion(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mInertialModel.getAmbientMotion(parameterValueType);
}

vec3 PhysicalModelImpl::getParameterRotation(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    glm::vec3 rotationRadians;
    glm::extractEulerAngleXYZ(
            glm::mat4_cast(mInertialModel.getRotation(parameterValueType)),
            rotationRadians.x, rotationRadians.y, rotationRadians.z);
    return fromGlm(glm::degrees(rotationRadians));
}

vec3 PhysicalModelImpl::getParameterMagneticField(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return fromGlm(mAmbientEnvironment.getMagneticField(parameterValueType));
}

float PhysicalModelImpl::getParameterTemperature(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mAmbientEnvironment.getTemperature(parameterValueType);
}

float PhysicalModelImpl::getParameterProximity(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mAmbientEnvironment.getProximity(parameterValueType);
}

float PhysicalModelImpl::getParameterLight(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mAmbientEnvironment.getLight(parameterValueType);
}

float PhysicalModelImpl::getParameterPressure(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mAmbientEnvironment.getPressure(parameterValueType);
}

float PhysicalModelImpl::getParameterHumidity(
        ParameterValueType parameterValueType) const {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    return mAmbientEnvironment.getHumidity(parameterValueType);
}

#define GET_FUNCTION_NAME(x) get##x
#define OVERRIDE_FUNCTION_NAME(x) override##x
#define OVERRIDE_NAME(x) m##x##Override
#define SENSOR_NAME(x) ANDROID_SENSOR_##x
#define PHYSICAL_NAME(x) getPhysical##x

// Implement sensor overrides.
#define SENSOR_(x,y,z,v,w) \
void PhysicalModelImpl::OVERRIDE_FUNCTION_NAME(z)(v override_value) {\
    setOverride(SENSOR_NAME(x), &OVERRIDE_NAME(z), override_value);\
}

SENSORS_LIST
#undef SENSOR_

// Implement getters that respect overrides.
#define SENSOR_(x,y,z,v,w) \
v PhysicalModelImpl::GET_FUNCTION_NAME(z)(\
        long* measurement_id) const {\
    return getSensorValue<v>(SENSOR_NAME(x),\
                             &OVERRIDE_NAME(z),\
                             std::bind(&PhysicalModelImpl::PHYSICAL_NAME(z),\
                                       this),\
                             measurement_id);\
}

SENSORS_LIST
#undef SENSOR_

#undef PHYSICAL_NAME
#undef SENSOR_NAME
#undef OVERRIDE_NAME
#undef OVERRIDE_FUNCTION_NAME
#undef GET_FUNCTION_NAME

vec3 PhysicalModelImpl::getPhysicalAccelerometer() const {
    //Implementation Note:
    // Gravity and magnetic vectors as observed by the device.
    // Note how we're applying the *inverse* of the transformation
    // represented by device_rotation_quat to the "absolute" coordinates
    // of the vectors.
    return fromGlm(glm::conjugate(mInertialModel.getRotation()) *
        (mInertialModel.getAcceleration() - mAmbientEnvironment.getGravity()));
}

vec3 PhysicalModelImpl::getPhysicalGyroscope() const {
    return fromGlm(glm::conjugate(mInertialModel.getRotation()) *
        mInertialModel.getRotationalVelocity());
}

vec3 PhysicalModelImpl::getPhysicalMagnetometer() const {
    return fromGlm(glm::conjugate(mInertialModel.getRotation()) *
        mAmbientEnvironment.getMagneticField());
}

/* (x, y, z) == (azimuth, pitch, roll) */
vec3 PhysicalModelImpl::getPhysicalOrientation() const {
    return fromGlm(glm::eulerAngles(mInertialModel.getRotation()));
}

float PhysicalModelImpl::getPhysicalTemperature() const {
    return mAmbientEnvironment.getTemperature();
}

float PhysicalModelImpl::getPhysicalProximity() const {
    return mAmbientEnvironment.getProximity();
}

float PhysicalModelImpl::getPhysicalLight() const {
    return mAmbientEnvironment.getLight();
}

float PhysicalModelImpl::getPhysicalPressure() const {
    return mAmbientEnvironment.getPressure();
}

float PhysicalModelImpl::getPhysicalHumidity() const {
    return mAmbientEnvironment.getHumidity();
}

vec3 PhysicalModelImpl::getPhysicalMagnetometerUncalibrated() const {
    return fromGlm(glm::conjugate(mInertialModel.getRotation()) *
        mAmbientEnvironment.getMagneticField());
}

vec3 PhysicalModelImpl::getPhysicalGyroscopeUncalibrated() const {
    return fromGlm(glm::conjugate(mInertialModel.getRotation()) *
        mInertialModel.getRotationalVelocity());
}

void PhysicalModelImpl::getTransform(
        float* out_translation_x, float* out_translation_y, float* out_translation_z,
        float* out_rotation_x, float* out_rotation_y, float* out_rotation_z,
        int64_t* out_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    const vec3 position = getParameterPosition(PARAMETER_VALUE_TYPE_CURRENT);
    *out_translation_x = position.x;
    *out_translation_y = position.y;
    *out_translation_z = position.z;
    const vec3 rotation = getParameterRotation(PARAMETER_VALUE_TYPE_CURRENT);
    *out_rotation_x = rotation.x;
    *out_rotation_y = rotation.y;
    *out_rotation_z = rotation.z;
    *out_timestamp = mModelTimeNs;

    if (mGroundTruthStream) {
        fprintf(mGroundTruthStream->get(), "%" PRId64 " %f %f %f %f %f %f\n",
                *out_timestamp, *out_translation_x, *out_translation_y,
                *out_translation_z, *out_rotation_x, *out_rotation_y,
                *out_rotation_z);
    }
}

void PhysicalModelImpl::setPhysicalStateAgent(
        const QAndroidPhysicalStateAgent* agent) {
    bool stateChanging = false;

    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        mAgent = agent;
        stateChanging = mIsPhysicalStateChanging;
    }

    // Invoke callbacks outside of the lock.
    if (agent) {
        if (stateChanging) {
            // Ensure the new agent is set correctly if the is a pending state
            // change.
            if (agent->onPhysicalStateChanging) {
                agent->onPhysicalStateChanging(agent->context);
            }
        } else {
            // If no state change is pending, we still send a change/stabilize
            // message so that agents can depend on them for initialization.
            if (agent->onPhysicalStateChanging) {
                agent->onPhysicalStateChanging(agent->context);
            }
            if (agent->onPhysicalStateStabilized) {
                agent->onPhysicalStateStabilized(agent->context);
            }
        }

        // We send an initial target state change so agents can depend on this
        // for initialization.
        if (agent->onTargetStateChanged) {
            agent->onTargetStateChanged(agent->context);
        }
    }
}

void PhysicalModelImpl::setAutomationController(
        AutomationController* controller) {
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    mAutomationController = controller;
}

static void readValueFromStream(Stream* f, vec3* value) {
    value->x = stream_get_float(f);
    value->y = stream_get_float(f);
    value->z = stream_get_float(f);
}

static void readValueFromStream(Stream* f, float* value) {
    *value = stream_get_float(f);
}

static void writeValueToStream(Stream* f, vec3 value) {
    stream_put_float(f, value.x);
    stream_put_float(f, value.y);
    stream_put_float(f, value.z);
}

static void writeValueToStream(Stream* f, float value) {
    stream_put_float(f, value);
}

void PhysicalModelImpl::snapshotSave(Stream* f) {
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    // first save targets
    stream_put_be32(f, MAX_PHYSICAL_PARAMETERS);

    for (int parameter = 0;
         parameter < MAX_PHYSICAL_PARAMETERS;
         parameter++) {
        switch (parameter) {
#define PHYSICAL_PARAMETER_NAME(x) PHYSICAL_PARAMETER_##x
#define GET_PARAMETER_FUNCTION_NAME(x) getParameter##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
            case PHYSICAL_PARAMETER_NAME(x): {\
                w targetValue = GET_PARAMETER_FUNCTION_NAME(z)(\
                        PARAMETER_VALUE_TYPE_TARGET);\
                writeValueToStream(f, targetValue);\
                break;\
            }

            PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef GET_PARAMETER_FUNCTION_NAME
#undef PHYSICAL_PARAMETER_NAME
            default:
                assert(false);  // should never happen
                break;
        }
    }

    // then save overrides
    stream_put_be32(f, MAX_SENSORS);

    for (int sensor = 0;
         sensor < MAX_SENSORS;
         sensor++) {
        stream_put_be32(f, mUseOverride[sensor]);
        if (mUseOverride[sensor]) {
            switch(sensor) {
#define SENSOR_NAME(x) ANDROID_SENSOR_##x
#define OVERRIDE_NAME(x) m##x##Override
#define SENSOR_(x,y,z,v,w)\
                case SENSOR_NAME(x): {\
                    writeValueToStream(f, OVERRIDE_NAME(z));\
                    break;\
                }

                SENSORS_LIST
#undef SENSOR_
#undef OVERRIDE_NAME
#undef SENSOR_NAME
                default:
                    assert(false);  // should never happen
                    break;
            }
        }
    }
}

int PhysicalModelImpl::snapshotLoad(Stream* f) {
    // first load targets
    const int32_t num_physical_parameters = stream_get_be32(f);
    if (num_physical_parameters > MAX_PHYSICAL_PARAMETERS) {
        D("%s: cannot load: snapshot requires %d physical parameters, %d "
          "available\n",
          __FUNCTION__, num_physical_parameters, MAX_PHYSICAL_PARAMETERS);
        return -EIO;
    }

    // Note: any new target params will remain at their defaults.

    for (int parameter = 0;
         parameter < num_physical_parameters;
         parameter++) {
        switch (parameter) {
#define PHYSICAL_PARAMETER_NAME(x) PHYSICAL_PARAMETER_##x
#define SET_TARGET_FUNCTION_NAME(x) setTargetInternal##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
            case PHYSICAL_PARAMETER_NAME(x): {\
                w value;\
                readValueFromStream(f, &value);\
                SET_TARGET_FUNCTION_NAME(z)(\
                        value, PHYSICAL_INTERPOLATION_STEP);\
                break;\
            }

            PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef SET_TARGET_FUNCTION_NAME
#undef PHYSICAL_PARAMETER_NAME
            default:
                assert(false);  // should never happen
                break;
        }
    }

    // then load overrides

    /* check number of physical sensors */
    int32_t num_sensors = stream_get_be32(f);
    if (num_sensors > MAX_SENSORS) {
        D("%s: cannot load: snapshot requires %d physical sensors, %d "
          "available\n",
          __FUNCTION__, num_sensors, MAX_SENSORS);
        return -EIO;
    }

    for (int sensor = 0;
         sensor < num_sensors;
         sensor++) {
        if (stream_get_be32(f)) {
            switch(sensor) {
#define SENSOR_NAME(x) ANDROID_SENSOR_##x
#define OVERRIDE_FUNCTION_NAME(x) override##x
#define SENSOR_(x,y,z,v,w)\
                case SENSOR_NAME(x): {\
                    v value;\
                    readValueFromStream(f, &value);\
                    OVERRIDE_FUNCTION_NAME(z)(value);\
                    break;\
                }

                SENSORS_LIST
#undef SENSOR_
#undef OVERRIDE_FUNCTION_NAME
#undef SENSOR_NAME
                default:
                    assert(false);  // should never happen
                    break;
            }
        }
    }

    return 0;
}

template <typename ValueType>
void serializeState(pb::InitialState* state, PhysicalParameter type,
                    ValueType currentValue, ValueType targetValue) {
    pb::PhysicalModelEvent* command = state->add_physical_model();
    command->set_type(toProto(type));
    setProtoCurrentValue(command, currentValue);
    setProtoTargetValue(command, targetValue);
}

void PhysicalModelImpl::saveState(pb::InitialState* state) {
    std::lock_guard<std::recursive_mutex> lock(mMutex);

    state->mutable_initial_time()->set_timestamp(mModelTimeNs);

    // TODO(jwmcglynn): Prune state and only save non-default parameters.

    for (int parameter = 0; parameter < MAX_PHYSICAL_PARAMETERS; parameter++) {
        switch (parameter) {
#define PHYSICAL_PARAMETER_NAME(x) PHYSICAL_PARAMETER_##x
#define GET_PARAMETER_FUNCTION_NAME(x) getParameter##x
#define PHYSICAL_PARAMETER_(x, y, z, w)            \
    case PHYSICAL_PARAMETER_NAME(x): {             \
        serializeState(                            \
                state, PHYSICAL_PARAMETER_NAME(x), \
                GET_PARAMETER_FUNCTION_NAME(z)(PARAMETER_VALUE_TYPE_CURRENT_NO_AMBIENT_MOTION),  \
                GET_PARAMETER_FUNCTION_NAME(z)(PARAMETER_VALUE_TYPE_TARGET)); \
            break;                                                            \
        }

        PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef GET_PARAMETER_FUNCTION_NAME
#undef PHYSICAL_PARAMETER_NAME
            default:
                assert(false);  // should never happen
                break;
        }
    }
}

void PhysicalModelImpl::loadState(const pb::InitialState& state) {
    constexpr vec3 kVecZero = {0.f, 0.f, 0.f};
    vec3 currentPosition = {0.f, 0.f, 0.f};
    vec3 targetPosition = {0.f, 0.f, 0.f};
    vec3 currentVelocity = {0.f, 0.f, 0.f};
    vec3 targetVelocity = {0.f, 0.f, 0.f};

    for (int i = 0; i < state.physical_model_size(); ++i) {
        const pb::PhysicalModelEvent& event = state.physical_model(i);

        // Position and velocity require special ordering to replay properly,
        // dispatch those events separately.
        if (event.type() == pb::PhysicalModelEvent_ParameterType_POSITION) {
            if (event.has_current_value()) {
                currentPosition = getProtoValue<vec3>(event.current_value());
            }
            if (event.has_target_value()) {
                targetPosition = getProtoValue<vec3>(event.target_value());
            }
        } else if (event.type() ==
                   pb::PhysicalModelEvent_ParameterType_VELOCITY) {
            if (event.has_current_value()) {
                currentVelocity = getProtoValue<vec3>(event.current_value());
            }
            if (event.has_target_value()) {
                targetVelocity = getProtoValue<vec3>(event.target_value());
            }
        } else {
            replayEvent(event);
        }
    }

    // To avoid overriding the velocity, first replay current position, then
    // current velocity, then the target position and velocity.
    setTargetInternalPosition(currentPosition, PHYSICAL_INTERPOLATION_STEP);
    setTargetInternalVelocity(currentVelocity, PHYSICAL_INTERPOLATION_STEP);

    if (targetVelocity != kVecZero) {
        // If we have a non-zero target velocity, then we are moving at a
        // velocity instead of towards a position.
        setTargetInternalVelocity(targetVelocity,
                                  PHYSICAL_INTERPOLATION_SMOOTH);
    } else {
        setTargetInternalPosition(targetPosition,
                                  PHYSICAL_INTERPOLATION_SMOOTH);
    }
}

int PhysicalModelImpl::recordGroundTruth(const char* filename) {
    stopRecordGroundTruth();

    if (!filename) {
        E("%s: Must specify filename for writing.  Physical Motion will not be "
          "recorded.",
          __FUNCTION__, filename);
        return -1;
    }

    std::string path = filename;
    if (!PathUtils::isAbsolute(path)) {
        path = PathUtils::join(System::get()->getHomeDirectory(), filename);
    }

    mGroundTruthStream.reset(
            new StdioStream(fopen(path.c_str(), "wb"), StdioStream::kOwner));
    if (!mGroundTruthStream.get()) {
        E("%s: Error unable to open file %s for writing.  "
          "Physical motion ground truth will not be recorded.",
          __FUNCTION__, filename);
        return -1;
    }

    return 0;
}

void PhysicalModelImpl::stopRecordGroundTruth() {
    mGroundTruthStream.reset(nullptr);
}

#define SET_TARGET_FUNCTION_NAME(x) setTarget##x
#define SET_TARGET_INTERNAL_FUNCTION_NAME(x) setTargetInternal##x
#define PHYSICAL_PARAMETER_ENUM(x) PHYSICAL_PARAMETER_##x
#define PHYSICAL_PARAMETER_(x, y, z, w)                         \
    void PhysicalModelImpl::SET_TARGET_FUNCTION_NAME(z)(        \
            w value, PhysicalInterpolation mode) {              \
        generateEvent(PHYSICAL_PARAMETER_ENUM(x), mode, value); \
        SET_TARGET_INTERNAL_FUNCTION_NAME(z)(value, mode);      \
    }

PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef PHYSICAL_PARAMETER_ENUM
#undef SET_TARGET_INTERNAL_FUNCTION_NAME
#undef SET_TARGET_FUNCTION_NAME

void PhysicalModelImpl::physicalStateChanging() {
    const QAndroidPhysicalStateAgent* agent = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        // Note: We only call onPhysicalStateChanging if this is a transition
        // from stable to changing (i.e. don't call if we get to
        // physicalStateChanging calls in a row without a
        // physicalStateStabilized call in between).
        if (!mIsPhysicalStateChanging) {
            mIsPhysicalStateChanging = true;
            agent = mAgent;
        }
    }

    // Call the callback outside of the lock.
    if (agent && agent->onPhysicalStateChanging) {
        agent->onPhysicalStateChanging(agent->context);
    }
}

void PhysicalModelImpl::physicalStateStabilized() {
    const QAndroidPhysicalStateAgent* agent;
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        assert(mIsPhysicalStateChanging);
        agent = mAgent;

        // Increment all of the measurement ids because the physical state has
        // stabilized.
        for (size_t i = 0; i < MAX_SENSORS; i++) {
            mMeasurementId[i]++;
        }
        mIsPhysicalStateChanging = false;
    }

    // Call the callback outside of the lock.
    if (agent && agent->onPhysicalStateStabilized) {
        agent->onPhysicalStateStabilized(agent->context);
    }
}

void PhysicalModelImpl::targetStateChanged() {
    const QAndroidPhysicalStateAgent* agent;
    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        // When target state changes we reset all sensor overrides.
        for (size_t i = 0; i < MAX_SENSORS; ++i) {
            mUseOverride[i] = false;
        }

        agent = mAgent;
    }

    // Call the callback outside of the lock.
    if (agent && agent->onTargetStateChanged) {
        agent->onTargetStateChanged(agent->context);
    }
}

template <typename ValueType>
void PhysicalModelImpl::generateEvent(PhysicalParameter type,
                   PhysicalInterpolation mode,
                   ValueType value) {
    pb::PhysicalModelEvent command;
    command.set_type(toProto(type));
    if (mode == PHYSICAL_INTERPOLATION_SMOOTH) {
        setProtoTargetValue(&command, value);
    } else {
        setProtoCurrentValue(&command, value);
    }

    if (mAutomationController) {
        mAutomationController->getEventSink().recordPhysicalModelEvent(
                mModelTimeNs, command);
    }
}

}  // namespace physics
}  // namespace android

using android::physics::PhysicalModelImpl;

PhysicalModel* physicalModel_new() {
    PhysicalModelImpl* impl = new PhysicalModelImpl();
    return impl != nullptr ? impl->getPhysicalModel() : nullptr;
}

void physicalModel_free(PhysicalModel* model) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        delete impl;
    }
}

void physicalModel_setCurrentTime(
        PhysicalModel* model, int64_t time_ns) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->setCurrentTime(time_ns);
    }
}

#define SET_PHYSICAL_TARGET_FUNCTION_NAME(x) physicalModel_setTarget##x
#define SET_TARGET_FUNCTION_NAME(x) setTarget##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
void SET_PHYSICAL_TARGET_FUNCTION_NAME(z)(\
        PhysicalModel* model, w value, PhysicalInterpolation mode) {\
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);\
    if (impl != nullptr) {\
        impl->SET_TARGET_FUNCTION_NAME(z)(value, mode);\
    }\
}

PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef SET_TARGET_FUNCTION_NAME
#undef SET_PHYSICAL_TARGET_FUNCTION_NAME


#define GET_PHYSICAL_PARAMETER_FUNCTION_NAME(x) physicalModel_getParameter##x
#define GET_PARAMETER_FUNCTION_NAME(x) getParameter##x
#define PHYSICAL_PARAMETER_(x,y,z,w) \
w GET_PHYSICAL_PARAMETER_FUNCTION_NAME(z)(\
        PhysicalModel* model, ParameterValueType parameterValueType) {\
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);\
    w result;\
    if (impl != nullptr) {\
        result = impl->GET_PARAMETER_FUNCTION_NAME(z)(parameterValueType);\
    } else {\
        result = {0.f};\
    }\
    return result;\
}

PHYSICAL_PARAMETERS_LIST
#undef PHYSICAL_PARAMETER_
#undef GET_PARAMETER_FUNCTION_NAME
#undef GET_PHYSICAL_PARAMETER_FUNCTION_NAME

#define OVERRIDE_FUNCTION_NAME(x) override##x
#define PHYSICAL_OVERRIDE_FUNCTION_NAME(x) physicalModel_override##x
#define SENSOR_(x,y,z,v,w) \
void PHYSICAL_OVERRIDE_FUNCTION_NAME(z)(\
        PhysicalModel* model,\
        v override_value) {\
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);\
    if (impl != nullptr) {\
        impl->OVERRIDE_FUNCTION_NAME(z)(override_value);\
    }\
}

SENSORS_LIST
#undef SENSOR_
#undef PHYSICAL_OVERRIDE_FUNCTION_NAME
#undef OVERRIDE_FUNCTION_NAME

#define GET_FUNCTION_NAME(x) get##x
#define PHYSICAL_GET_FUNCTION_NAME(x) physicalModel_get##x
#define SENSOR_(x,y,z,v,w) \
v PHYSICAL_GET_FUNCTION_NAME(z)(\
        PhysicalModel* model, long* measurement_id) {\
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);\
    *measurement_id = 0L;\
    return impl != nullptr ?\
            impl->GET_FUNCTION_NAME(z)(measurement_id) :\
            v{0.f};\
}

SENSORS_LIST
#undef SENSOR_
#undef PHYSICAL_GET_FUNCTION_NAME
#undef GET_FUNCTION_NAME

void physicalModel_getTransform(PhysicalModel* model,
    float* out_translation_x, float* out_translation_y, float* out_translation_z,
    float* out_rotation_x, float* out_rotation_y, float* out_rotation_z,
    int64_t* out_timestamp) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->getTransform(out_translation_x, out_translation_y, out_translation_z,
                out_rotation_x, out_rotation_y, out_rotation_z,
                out_timestamp);
    }
}

void physicalModel_setPhysicalStateAgent(PhysicalModel* model,
        const QAndroidPhysicalStateAgent* agent) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->setPhysicalStateAgent(agent);
    }
}

void physicalModel_setAutomationController(PhysicalModel* model,
                                           AutomationController* controller) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->setAutomationController(controller);
    }
}

void physicalModel_snapshotSave(PhysicalModel* model, Stream* f) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->snapshotSave(f);
    }
}

int physicalModel_snapshotLoad(PhysicalModel* model, Stream* f) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        return impl->snapshotLoad(f);
    } else {
        return -EIO;
    }
}

int physicalModel_saveState(PhysicalModel* model, pb::InitialState* state) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->saveState(state);
        return 0;
    } else {
        return -EIO;
    }
}

int physicalModel_loadState(PhysicalModel* model, const pb::InitialState& state) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->loadState(state);
        return 0;
    } else {
        return -EIO;
    }
}

void physicalModel_replayEvent(PhysicalModel* model,
                               const pb::PhysicalModelEvent& event) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->replayEvent(event);
    } else {
        D("%s: Discarding sensor event", __FUNCTION__);
    }
}

int physicalModel_recordGroundTruth(PhysicalModel* model,
                                    const char* filename) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        return impl->recordGroundTruth(filename);
    }
    return -1;
}

int physicalModel_stopRecording(PhysicalModel* model) {
    PhysicalModelImpl* impl = PhysicalModelImpl::getImpl(model);
    if (impl != nullptr) {
        impl->stopRecordGroundTruth();
        return 0;
    }
    return -1;
}
