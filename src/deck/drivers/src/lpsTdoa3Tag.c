/*
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie firmware.
 *
 * Copyright 2018, Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * lpsTdoa3Tag.c is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with lpsTdoa3Tag.c. If not, see <http://www.gnu.org/licenses/>.
 */


/*

The tag is assumed to move around in a large system of anchors. Any anchor ids
can be used, and the same anchor id can even be used by multiple anchors as long
as they are not visible in the same area. It is assumed that the anchor density
is evenly distributed in the covered volume and that 5-20 anchors are visible
in every point. The tag is attached to a physical object and the expected
velocity is a few m/s, this means that anchors are within range for a time
period of seconds.

The implementation must handle
1. An infinite number of anchors, where around 20 are visible at one time
2. Any anchor ids
3. Dynamically changing visibility of anchors over time
4. Random TX times from anchors with possible packet collisions and packet loss

*/

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lpsTdoa3Tag.h"
#include "tdoaEngineInstance.h"
#include "tdoaStats.h"
#include "estimator.h"

#include "libdw1000.h"
#include "mac.h"

#define DEBUG_MODULE "TDOA3"
#include "debug.h"
#include "cfassert.h"

// Positions for sent LPP packets
#define LPS_TDOA3_TYPE 0
#define LPS_TDOA3_SEND_LPP_PAYLOAD 1

#define PACKET_TYPE_TDOA3 0x30

#define TDOA3_RECEIVE_TIMEOUT 10000


#define ANTENNA_OFFSET  154.6


typedef struct {
  uint8_t type;
  uint8_t seq;
  uint32_t txTimeStamp;
  uint8_t remoteCount;
} __attribute__((packed)) rangePacketHeader3_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
  uint16_t distance;
} __attribute__((packed)) remoteAnchorDataFull_t;

typedef struct {
  uint8_t id;
  uint8_t seq;
  uint32_t rxTimeStamp;
} __attribute__((packed)) remoteAnchorDataShort_t;

typedef struct {
  rangePacketHeader3_t header;
  uint8_t remoteAnchorData;
} __attribute__((packed)) rangePacket3_t;

// log tdoa3 d12 data
static float log_tdoa3_d12 = 0.0f;
static float log_snr_1 = 0.0f;          // FP Amplitude / CIRE noiseStd from anchor 1
static float log_snr_2 = 0.0f;          // FP Amplitude / CIRE noiseStd from anchor 2
static float log_powerdiff_1 = 0.0f;    // RX_POWER - FP_POWER from anchor 1
static float log_powerdiff_2 = 0.0f;    // RX_POWER - FP_POWER from anchor 2

static float log_anchor1_RX_snr = 0.0f;       // anchor1 received snr from anchor2
static float log_anchor1_RX_powerdif = 0.0f;  // anchor1 received power diff from anchor2
static float log_anchor2_RX_snr = 0.0f;       // anchor2 received snr from anchor1
static float log_anchor2_RX_powerdif = 0.0f;  // anchor2 received power diff from anchor1

static float log_anchor1_tof = 0.0f;    // the tof from anchor 1
static float log_anchor2_tof = 0.0f;    // the tof from anchor 2

// Outgoing LPP packet
static lpsLppShortPacket_t lppPacket;

static bool rangingOk;

static bool isValidTimeStamp(const int64_t anchorRxTime) {
  return anchorRxTime != 0;
}

static int updateRemoteData(tdoaAnchorContext_t* anchorCtx, const void* payload) {
  const rangePacket3_t* packet = (rangePacket3_t*)payload;
  const void* anchorDataPtr = &packet->remoteAnchorData;
  for (uint8_t i = 0; i < packet->header.remoteCount; i++) {
    remoteAnchorDataFull_t* anchorData = (remoteAnchorDataFull_t*)anchorDataPtr;

    uint8_t remoteId = anchorData->id;
    int64_t remoteRxTime = anchorData->rxTimeStamp;
    uint8_t remoteSeqNr = anchorData->seq & 0x7f;

    if (isValidTimeStamp(remoteRxTime)) {
      tdoaStorageSetRemoteRxTime(anchorCtx, remoteId, remoteRxTime, remoteSeqNr);
    }

    bool hasDistance = ((anchorData->seq & 0x80) != 0);
    if (hasDistance) {
      // send uint16_t to int64_t. Because it doesn't lose the accuracy here.
      int64_t tof = anchorData->distance;
      if (isValidTimeStamp(tof)) {
        tdoaStorageSetTimeOfFlight(anchorCtx, remoteId, tof);

        uint8_t anchorId = tdoaStorageGetId(anchorCtx);
        tdoaStats_t* stats = &tdoaEngineState.stats;
        if (anchorId == stats->anchorId && remoteId == stats->remoteAnchorId) {
          stats->tof = (uint16_t)tof;  // the unit is in radio tick
        }
        //--------------------------- change --------------------------------//
        // compute the tof distance (in meter)
        //  M_PER_TICK = SPEED_OF_LIGHT / LOCODECK_TS_FREQ
        //  precompute value
        double M_PER_TICK = 0.0046917639786157855; 
        // check anchorId
        if(anchorId ==(uint8_t)1){
            // tof meas. carried in anchor1's packet (in meter)
            log_anchor1_tof = (uint16_t)tof* M_PER_TICK - ANTENNA_OFFSET;
        }
        if(anchorId ==(uint8_t)2){
            // tof meas. carried in anchor2's packet (in meter)
            log_anchor2_tof = (uint16_t)tof* M_PER_TICK - ANTENNA_OFFSET;
        }
      }

      anchorDataPtr += sizeof(remoteAnchorDataFull_t);
    } else {
      anchorDataPtr += sizeof(remoteAnchorDataShort_t);
    }
  }

  return (uint8_t*)anchorDataPtr - (uint8_t*)packet;
}

static void handleLppShortPacket(tdoaAnchorContext_t* anchorCtx, const uint8_t *data, const int length, uint8_t anchorId) {
  uint8_t type = data[0];

  if (type == LPP_SHORT_ANCHORPOS) {
    struct lppShortAnchorPos_s *newpos = (struct lppShortAnchorPos_s*)&data[1];
    tdoaStorageSetAnchorPosition(anchorCtx, newpos->x, newpos->y, newpos->z);
    // receive the power values
    // check anchor id
    // [Note]: check if anchorId == tdoaStorageGetId(anchorCtx)
    // If yes, we don't need to send anchorId in
    if(anchorId ==(uint8_t)1){
        log_anchor1_RX_snr = newpos->snr;
        log_anchor1_RX_powerdif = newpos->power_diff;
    }
    if(anchorId ==(uint8_t)2){
        log_anchor2_RX_snr = newpos->snr;
        log_anchor2_RX_powerdif = newpos->power_diff;
    }
  }
}

static void handleLppPacket(const int dataLength, int rangePacketLength, const packet_t* rxPacket, tdoaAnchorContext_t* anchorCtx, uint8_t anchorId) {
  const int32_t payloadLength = dataLength - MAC802154_HEADER_LENGTH;
  const int32_t startOfLppDataInPayload = rangePacketLength;
  const int32_t lppDataLength = payloadLength - startOfLppDataInPayload;
  const int32_t lppTypeInPayload = startOfLppDataInPayload + 1;

  if (lppDataLength > 0) {
    const uint8_t lppPacketHeader = rxPacket->payload[startOfLppDataInPayload];
    if (lppPacketHeader == LPP_HEADER_SHORT_PACKET) {
      const int32_t lppTypeAndPayloadLength = lppDataLength - 1;
      // send anchor id
      handleLppShortPacket(anchorCtx, &rxPacket->payload[lppTypeInPayload], lppTypeAndPayloadLength, anchorId);
    }
  }
}

static void rxcallback(dwDevice_t *dev) {
  tdoaStats_t* stats = &tdoaEngineState.stats;
  STATS_CNT_RATE_EVENT(&stats->packetsReceived);

  int dataLength = dwGetDataLength(dev);
  packet_t rxPacket;

  dwGetData(dev, (uint8_t*)&rxPacket, dataLength);
  const uint8_t anchorId = rxPacket.sourceAddress & 0xff;
  // get the (1) first path power:              FP_POWER,
  //         (2) total received power:          RX_POWER,
  //     and (3) FP Amplitude / CIRE noiseStd:  snr
  float RX_POWER = 0.0f;    // received power
  float FP_POWER = 0.0f;    // first path power
  FP_POWER = dwGetFirstPathPower(dev);
  RX_POWER = dwGetReceivePower(dev);
  if(anchorId==(uint8_t)1)
  {
      log_snr_1 = dwGetReceiveQuality(dev);
      log_powerdiff_1 = RX_POWER - FP_POWER;
  }
  else if (anchorId==(uint8_t)2)
  {
      log_snr_2 = dwGetReceiveQuality(dev);
      log_powerdiff_2 = RX_POWER - FP_POWER;
  }

  dwTime_t arrival = {.full = 0};
  dwGetReceiveTimestamp(dev, &arrival);
  const int64_t rxAn_by_T_in_cl_T = arrival.full;

  const rangePacket3_t* packet = (rangePacket3_t*)rxPacket.payload;
  if (packet->header.type == PACKET_TYPE_TDOA3) {
    const int64_t txAn_in_cl_An = packet->header.txTimeStamp;;
    const uint8_t seqNr = packet->header.seq & 0x7f;;

    tdoaAnchorContext_t anchorCtx;
    uint32_t now_ms = T2M(xTaskGetTickCount());

    tdoaEngineGetAnchorCtxForPacketProcessing(&tdoaEngineState, anchorId, now_ms, &anchorCtx);
    // [change]: send the anchor id
    int rangeDataLength = updateRemoteData(&anchorCtx, packet);
    tdoaEngineProcessPacket(&tdoaEngineState, &anchorCtx, txAn_in_cl_An, rxAn_by_T_in_cl_T);

    tdoaStorageSetRxTxData(&anchorCtx, rxAn_by_T_in_cl_T, txAn_in_cl_An, seqNr);
    // [change]: send the anchor id
    handleLppPacket(dataLength, rangeDataLength, &rxPacket, &anchorCtx, anchorId);

    rangingOk = true;
  }
}

static void setRadioInReceiveMode(dwDevice_t *dev) {
  dwNewReceive(dev);
  dwSetDefaults(dev);
  dwStartReceive(dev);
}

static void sendLppShort(dwDevice_t *dev, lpsLppShortPacket_t *packet)
{
  static packet_t txPacket;
  dwIdle(dev);

  MAC80215_PACKET_INIT(txPacket, MAC802154_TYPE_DATA);

  txPacket.payload[LPS_TDOA3_TYPE] = LPP_HEADER_SHORT_PACKET;
  memcpy(&txPacket.payload[LPS_TDOA3_SEND_LPP_PAYLOAD], packet->data, packet->length);

  txPacket.pan = 0xbccf;
  txPacket.sourceAddress = 0xbccf000000000000 | 0xff;
  txPacket.destAddress = 0xbccf000000000000 | packet->dest;

  dwNewTransmit(dev);
  dwSetDefaults(dev);
  dwSetData(dev, (uint8_t*)&txPacket, MAC802154_HEADER_LENGTH+1+packet->length);

  dwStartTransmit(dev);
}

static bool sendLpp(dwDevice_t *dev) {
  bool lppPacketToSend = lpsGetLppShort(&lppPacket);
  if (lppPacketToSend) {
    sendLppShort(dev, &lppPacket);
    return true;
  }

  return false;
}

static uint32_t onEvent(dwDevice_t *dev, uwbEvent_t event) {
  switch(event) {
    case eventPacketReceived:
      rxcallback(dev);
      break;
    case eventTimeout:
      break;
    case eventReceiveTimeout:
      break;
    case eventPacketSent:
      // Service packet sent, the radio is back to receive automatically
      break;
    default:
      ASSERT_FAILED();
  }

  if(!sendLpp(dev)) {
    setRadioInReceiveMode(dev);
  }

  uint32_t now_ms = T2M(xTaskGetTickCount());
  tdoaStatsUpdate(&tdoaEngineState.stats, now_ms);

  return MAX_TIMEOUT;
}

static void sendTdoaToEstimatorCallback(tdoaMeasurement_t* tdoaMeasurement) {
  estimatorEnqueueTDOA(tdoaMeasurement);

  #ifdef LPS_2D_POSITION_HEIGHT
  // If LPS_2D_POSITION_HEIGHT is defined we assume that we are doing 2D positioning.
  // LPS_2D_POSITION_HEIGHT contains the height (Z) that the tag will be located at
  heightMeasurement_t heightData;
  heightData.timestamp = xTaskGetTickCount();
  heightData.height = LPS_2D_POSITION_HEIGHT;
  heightData.stdDev = 0.0001;
  estimatorEnqueueAbsoluteHeight(&heightData);
  #endif
    // change
    // For signal testing, log the TDOA3 data between anchor 1 and anchor 2
    const uint8_t idA = tdoaMeasurement->anchorIds[0];
    const uint8_t idB = tdoaMeasurement->anchorIds[1];
    if (idA==(uint8_t)1 && idB == (uint8_t)2)
    {
        log_tdoa3_d12 = tdoaMeasurement->distanceDiff;
    }

}

static bool getAnchorPosition(const uint8_t anchorId, point_t* position) {
  tdoaAnchorContext_t anchorCtx;
  uint32_t now_ms = T2M(xTaskGetTickCount());

  bool contextFound = tdoaStorageGetAnchorCtx(tdoaEngineState.anchorInfoArray, anchorId, now_ms, &anchorCtx);
  if (contextFound) {
    tdoaStorageGetAnchorPosition(&anchorCtx, position);
    return true;
  }

  return false;
}

static uint8_t getAnchorIdList(uint8_t unorderedAnchorList[], const int maxListSize) {
  return tdoaStorageGetListOfAnchorIds(tdoaEngineState.anchorInfoArray, unorderedAnchorList, maxListSize);
}

static uint8_t getActiveAnchorIdList(uint8_t unorderedAnchorList[], const int maxListSize) {
  uint32_t now_ms = T2M(xTaskGetTickCount());
  return tdoaStorageGetListOfActiveAnchorIds(tdoaEngineState.anchorInfoArray, unorderedAnchorList, maxListSize, now_ms);
}

static void Initialize(dwDevice_t *dev) {
  uint32_t now_ms = T2M(xTaskGetTickCount());
  tdoaEngineInit(&tdoaEngineState, now_ms, sendTdoaToEstimatorCallback, LOCODECK_TS_FREQ, TdoaEngineMatchingAlgorithmRandom);

  #ifdef LPS_2D_POSITION_HEIGHT
  DEBUG_PRINT("2D positioning enabled at %f m height\n", LPS_2D_POSITION_HEIGHT);
  #endif

  dwSetReceiveWaitTimeout(dev, TDOA3_RECEIVE_TIMEOUT);

  dwCommitConfiguration(dev);

  rangingOk = false;
}

static bool isRangingOk()
{
  return rangingOk;
}

uwbAlgorithm_t uwbTdoa3TagAlgorithm = {
  .init = Initialize,
  .onEvent = onEvent,
  .isRangingOk = isRangingOk,
  .getAnchorPosition = getAnchorPosition,
  .getAnchorIdList = getAnchorIdList,
  .getActiveAnchorIdList = getActiveAnchorIdList,
};



LOG_GROUP_START(tdoa3)
LOG_ADD(LOG_FLOAT, d1-2,        &log_tdoa3_d12)
LOG_ADD(LOG_FLOAT, snr_1,       &log_snr_1)
LOG_ADD(LOG_FLOAT, snr_2,       &log_snr_2)
LOG_ADD(LOG_FLOAT, powerdiff_1, &log_powerdiff_1)
LOG_ADD(LOG_FLOAT, powerdiff_2, &log_powerdiff_2)

LOG_ADD(LOG_FLOAT, an1_rx_snr,        &log_anchor1_RX_snr)
LOG_ADD(LOG_FLOAT, an1_rx_powerdif,   &log_anchor1_RX_powerdif)
LOG_ADD(LOG_FLOAT, an2_rx_snr,        &log_anchor2_RX_snr)
LOG_ADD(LOG_FLOAT, an2_rx_powerdif,   &log_anchor2_RX_powerdif)
LOG_ADD(LOG_FLOAT, an1_tof,           &log_anchor1_tof)
LOG_ADD(LOG_FLOAT, an2_tof,           &log_anchor2_tof)
LOG_GROUP_STOP(tdoa3)
