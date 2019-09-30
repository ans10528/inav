/*
 * This file is part of INAV.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License Version 3, as described below:
 *
 * This file is free software: you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */
#include "common/utils.h"
#include "flight/secondary_imu.h"
#include "config/parameter_group_ids.h"
#include "sensors/boardalignment.h"

#include "build/debug.h"

#include "drivers/sensor.h"
#include "drivers/accgyro/accgyro_bno055.h"

PG_REGISTER_WITH_RESET_TEMPLATE(secondaryImuConfig_t, secondaryImuConfig, PG_SECONDARY_IMU, 0);

PG_RESET_TEMPLATE(secondaryImuConfig_t, secondaryImuConfig,
    .enabled = 0,
    .rollDeciDegrees = 0,
    .pitchDeciDegrees = 0,
    .yawDeciDegrees = 0,
    .useForOsdHeading = 0,
    .useForOsdAHI = 0,
);

EXTENDED_FASTRAM secondaryImuState_t secondaryImuState;

void taskSecondaryImu(timeUs_t currentTimeUs)
{
    UNUSED(currentTimeUs);
    static bool secondaryImuChecked = false;

    if (!secondaryImuChecked) {
        secondaryImuState.active = bno055Init();
        secondaryImuChecked = true;
    }

    if (secondaryImuState.active) 
    {
        bno055FetchEulerAngles(secondaryImuState.eulerAngles.raw);

        //TODO this way of rotating a vector makes no sense, something simpler have to be developed
        const fpVector3_t v = {
            .x = secondaryImuState.eulerAngles.raw[0],
            .y = secondaryImuState.eulerAngles.raw[1],
            .z = secondaryImuState.eulerAngles.raw[2],
         };

        fpVector3_t rotated;

        fp_angles_t imuAngles = {
             .angles.roll = DECIDEGREES_TO_RADIANS(secondaryImuConfig()->rollDeciDegrees),
             .angles.pitch = DECIDEGREES_TO_RADIANS(secondaryImuConfig()->pitchDeciDegrees),
             .angles.yaw = DECIDEGREES_TO_RADIANS(secondaryImuConfig()->yawDeciDegrees),
        };
        fpMat3_t rotationMatrix;
        rotationMatrixFromAngles(&rotationMatrix, &imuAngles);
        rotationMatrixRotateVector(&rotated, &v, &rotationMatrix);
        rotated.z = ((int32_t)(rotated.z + secondaryImuConfig()->yawDeciDegrees)) % 3600;

        secondaryImuState.eulerAngles.values.roll = rotated.x;
        secondaryImuState.eulerAngles.values.pitch = rotated.y;
        secondaryImuState.eulerAngles.values.yaw = rotated.z;

        static uint8_t tick = 0;
        tick++;
        if (tick == 10) {
            secondaryImuState.calibrationStatus = bno055GetCalibStat();
            tick = 0;
        }

        DEBUG_SET(DEBUG_IMU2, 0, secondaryImuState.eulerAngles.values.roll);
        DEBUG_SET(DEBUG_IMU2, 1, secondaryImuState.eulerAngles.values.pitch);
        DEBUG_SET(DEBUG_IMU2, 2, secondaryImuState.eulerAngles.values.yaw);

        DEBUG_SET(DEBUG_IMU2, 3, secondaryImuState.calibrationStatus.mag);
        DEBUG_SET(DEBUG_IMU2, 4, secondaryImuState.calibrationStatus.gyr);
        DEBUG_SET(DEBUG_IMU2, 5, secondaryImuState.calibrationStatus.acc);
        

    }
}

