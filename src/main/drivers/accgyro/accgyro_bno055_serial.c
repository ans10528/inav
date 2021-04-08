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
#include "platform.h"
#ifdef USE_IMU_BNO055

#define BNO055_BAUD_RATE 115200
#define BNO055_FRAME_MAX_TIME_MS 10

#include <stdbool.h>
#include <stdint.h>
#include "io/serial.h"
#include "drivers/accgyro/accgyro_bno055.h"
#include "build/debug.h"
#include "drivers/time.h"
#include "flight/secondary_imu.h"

static serialPort_t * bno055SerialPort = NULL;

static uint8_t receiveBuffer[16];

typedef enum {
    BNO055_RECEIVE_IDLE,
    BNO055_RECEIVE_HEADER,
    BNO055_RECEIVE_LENGTH,
    BNO055_RECEIVE_PAYLOAD,
    BNO055_RECEIVE_ACK,
} bno055ReceiveState_e;

typedef enum {
    BNO055_FRAME_ACK,
    BNO055_FRAME_DATA,
} bno055FrameType_e;

typedef enum {
    BNO055_DATA_TYPE_NONE,
    BNO055_DATA_TYPE_EULER,
    BNO055_DATA_TYPE_CALIBRATION_STATS,
} bno055DataType_e;

static uint8_t bno055ProtocolState = BNO055_RECEIVE_IDLE;
static uint8_t bno055FrameType;
static uint8_t bno055FrameLength;
static uint8_t bno055FrameIndex;
static timeMs_t bno055FrameStartAtMs = 0;
static uint8_t bno055DataType = BNO055_DATA_TYPE_NONE;

static void bno055SerialWrite(uint8_t reg, uint8_t data) {
    bno055ProtocolState = BNO055_RECEIVE_IDLE;
    serialWrite(bno055SerialPort, 0xAA); // Start Byte
    serialWrite(bno055SerialPort, 0x00); // Write command
    serialWrite(bno055SerialPort, reg);
    serialWrite(bno055SerialPort, 1);
    serialWrite(bno055SerialPort, data);
}

static void bno055SerialRead(const uint8_t reg, const uint8_t len) {
    bno055ProtocolState = BNO055_RECEIVE_IDLE;
    serialWrite(bno055SerialPort, 0xAA); // Start Byte
    serialWrite(bno055SerialPort, 0x01); // Read command
    serialWrite(bno055SerialPort, reg);
    serialWrite(bno055SerialPort, len);
}

void bno055SerialDataReceive(uint16_t c, void *data) {

    UNUSED(data);

    const uint8_t incoming = (uint8_t) c;

    //Failsafe for stuck frames
    if (bno055ProtocolState != BNO055_RECEIVE_IDLE && millis() - bno055FrameStartAtMs > BNO055_FRAME_MAX_TIME_MS) {
        bno055ProtocolState = BNO055_RECEIVE_IDLE;
    }

    if (bno055ProtocolState == BNO055_RECEIVE_IDLE && incoming == 0xEE) {
        bno055FrameStartAtMs = millis();
        bno055ProtocolState = BNO055_RECEIVE_HEADER;
        bno055FrameType = BNO055_FRAME_ACK;
    } else if (bno055ProtocolState == BNO055_RECEIVE_IDLE && incoming == 0xBB) {
        bno055FrameStartAtMs = millis();
        bno055ProtocolState = BNO055_RECEIVE_HEADER;
        bno055FrameType = BNO055_FRAME_DATA;
    } else if (bno055ProtocolState == BNO055_RECEIVE_HEADER && bno055FrameType == BNO055_FRAME_ACK) {
        receiveBuffer[0] = incoming;
        bno055ProtocolState = BNO055_RECEIVE_IDLE;
    } else if (bno055ProtocolState == BNO055_RECEIVE_HEADER && bno055FrameType == BNO055_FRAME_DATA) {
        bno055FrameLength = incoming;
        bno055FrameIndex = 0;
        bno055ProtocolState = BNO055_RECEIVE_LENGTH;
    } else if (bno055ProtocolState == BNO055_RECEIVE_LENGTH) {
        receiveBuffer[bno055FrameIndex] = incoming;
        bno055FrameIndex++;

        if (bno055FrameIndex == bno055FrameLength) {
            bno055ProtocolState = BNO055_RECEIVE_IDLE;

            if (bno055DataType == BNO055_DATA_TYPE_EULER) {
                secondaryImuState.eulerAngles.raw[0] = ((int16_t)((receiveBuffer[3] << 8) | receiveBuffer[2])) / 1.6f;
                secondaryImuState.eulerAngles.raw[1] = ((int16_t)((receiveBuffer[5] << 8) | receiveBuffer[4])) / -1.6f; //Pitch has to be reversed to match INAV notation
                secondaryImuState.eulerAngles.raw[2] = ((int16_t)((receiveBuffer[1] << 8) | receiveBuffer[0])) / 1.6f;
                secondaryImuProcess();
            }  else if (bno055DataType == BNO055_DATA_TYPE_CALIBRATION_STATS) {
                secondaryImuState.calibrationStatus.mag = receiveBuffer[0] & 0b00000011;
                secondaryImuState.calibrationStatus.acc = (receiveBuffer[0] >> 2) & 0b00000011;
                secondaryImuState.calibrationStatus.gyr = (receiveBuffer[0] >> 4) & 0b00000011;
                secondaryImuState.calibrationStatus.sys = (receiveBuffer[0] >> 6) & 0b00000011;
            }

            bno055DataType = BNO055_DATA_TYPE_NONE;
        }
    }

}

bool bno055SerialInit(bno055CalibrationData_t calibrationData, bool setCalibration) {
    bno055SerialPort = NULL;

    serialPortConfig_t * portConfig = findSerialPortConfig(FUNCTION_BNO055);
    if (!portConfig) {
        return false;
    }

    bno055SerialPort = openSerialPort(
        portConfig->identifier,
        FUNCTION_BNO055,
        bno055SerialDataReceive,
        NULL,
        BNO055_BAUD_RATE,
        MODE_RXTX, 
        SERIAL_NOT_INVERTED | SERIAL_UNIDIR | SERIAL_STOPBITS_1 | SERIAL_PARITY_NO
    );
    
    if (!bno055SerialPort) {
        return false;
    }

    bno055SerialRead(0x00, 1); // Read ChipID
    delay(5);

    //Check ident
    if (bno055FrameType != BNO055_FRAME_DATA || receiveBuffer[0] != 0xA0 || bno055ProtocolState != BNO055_RECEIVE_IDLE) {
        return false; // Ident does not match, leave
    }

    bno055SerialWrite(BNO055_ADDR_PWR_MODE, BNO055_PWR_MODE_NORMAL); //Set power mode NORMAL
    delay(25);

    //TODO Here set calibration data 

    bno055SerialWrite(BNO055_ADDR_OPR_MODE, BNO055_OPR_MODE_NDOF); //Set power mode NORMAL
    delay(25);

    return true;
}

void bno055SerialFetchEulerAngles() {
    bno055DataType = BNO055_DATA_TYPE_EULER;
    bno055SerialRead(BNO055_ADDR_EUL_YAW_LSB, 6);
}

bno055CalibStat_t bno055SerialGetCalibStat(void) {
    bno055DataType = BNO055_DATA_TYPE_CALIBRATION_STATS;
    bno055SerialRead(BNO055_ADDR_CALIB_STAT, 1);
}

bno055CalibrationData_t bno055SerialGetCalibrationData(void) {

}

void bno055SerialSetCalibrationData(bno055CalibrationData_t data) {

}

#endif