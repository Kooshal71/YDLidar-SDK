//
// The MIT License (MIT)
//
// Copyright (c) 2019-2020 EAIBOT. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#include <core/serial/serial.h>
#include <core/network/ActiveSocket.h>
#include "YDlidarDriver.h"
#include <core/serial/common.h>
#include <math.h>
#include <ydlidar_config.h>
using namespace impl;

namespace ydlidar {
using namespace core::serial;
using namespace core::common;
using namespace core::network;

YDlidarDriver::YDlidarDriver(uint8_t type):
  _serial(NULL),
  m_TranformerType(type) {
  m_isConnected         = false;
  m_isScanning          = false;
  //串口配置参数
  m_intensities         = false;
  isAutoReconnect       = true;
  isAutoconnting        = false;
  m_baudrate            = 230400;
  m_SupportMotorDtrCtrl = true;
  m_HeartBeat           = false;
  scan_node_count       = 0;
  sample_rate           = 5000;
  m_PointTime           = 1e9 / 5000;
  trans_delay           = 0;
  scan_frequence        = 0;
  m_sampling_rate       = -1;
  model                 = -1;
  retryCount            = 0;
  has_device_header     = false;
  m_SingleChannel       = false;
  m_LidarType           = TYPE_TRIANGLE;

  //解析参数
  PackageSampleBytes    = 2;
  IntervalSampleAngle   = 0.0;
  FirstSampleAngle      = 0;
  LastSampleAngle       = 0;
  CheckSum              = 0;
  CheckSumCal           = 0;
  SampleNumlAndCTCal    = 0;
  LastSampleAngleCal    = 0;
  CheckSumResult        = true;
  Valu8Tou16            = 0;
  ct            = CT_Normal;
  nowPackageNum         = 0;
  package_Sample_Num    = 0;

  last_device_byte      = 0x00;
  asyncRecvPos          = 0;
  async_size            = 0;
  headerBuffer          = reinterpret_cast<uint8_t *>(&header_);
  infoBuffer            = reinterpret_cast<uint8_t *>(&info_);
  healthBuffer          = reinterpret_cast<uint8_t *>(&health_);
  package_Sample_Index  = 0;
  globalRecvBuffer      = new uint8_t[sizeof(tri_node_package)];
  scan_node_buf         = new node_info[MAX_SCAN_NODES];
  package_index         = 0;
  has_package_error     = false;

  get_device_health_success         = false;
  m_HasDeviceInfo           = false;
  IntervalSampleAngle_LastPackage   = 0.0;
  m_heartbeat_ts = getms();
  m_BlockRevSize = 0;
}

YDlidarDriver::~YDlidarDriver() 
{
  m_isScanning = false;
  isAutoReconnect = false;
  _thread.join();
  delay(200);

  {
    ScopedLocker l(_cmd_lock);
    if (_serial)
    {
      if (_serial->isOpen())
      {
        _serial->flush();
        _serial->closePort();
      }
    }
    if (_serial)
    {
      delete _serial;
      _serial = NULL;
    }
  }

  {
    ScopedLocker l(_lock);
    if (globalRecvBuffer)
    {
      delete[] globalRecvBuffer;
      globalRecvBuffer = NULL;
    }

    if (scan_node_buf)
    {
      delete[] scan_node_buf;
      scan_node_buf = NULL;
    }
  }
}

result_t YDlidarDriver::connect(const char *port_path, uint32_t baudrate) 
{
  m_baudrate = baudrate;
  serial_port = string(port_path);

  {
    ScopedLocker l(_cmd_lock);
    if (!_serial)
    {
      if (m_TranformerType == YDLIDAR_TYPE_TCP)
      {
        _serial = new CActiveSocket();
      }
      else
      {
        _serial = new serial::Serial(port_path, m_baudrate,
                                     serial::Timeout::simpleTimeout(DEFAULT_TIMEOUT));
      }
      _serial->bindport(port_path, baudrate);
    }

    if (!_serial->open())
    {
      setDriverError(NotOpenError);
      return RESULT_FAIL;
    }

    m_isConnected = true;
  }

  stopScan();
  delay(100);
  clearDTR();

  return RESULT_OK;
}

const char *YDlidarDriver::DescribeError(bool isTCP) {
  char const *value = "";

  if (_serial) {
    return _serial->DescribeError();
  }

  return value;
}


void YDlidarDriver::setDTR() {
  if (!m_isConnected) {
    return ;
  }

  if (_serial) {
    _serial->setDTR(1);
  }

}

void YDlidarDriver::clearDTR() {
  if (!m_isConnected) {
    return ;
  }

  if (_serial) {
    _serial->setDTR(0);
  }
}
void YDlidarDriver::flushSerial() {
  if (!m_isConnected) {
    return;
  }

  size_t len = _serial->available();

  if (len) {
    _serial->readSize(len);
  }

  delay(20);
}


void YDlidarDriver::disconnect() {
  isAutoReconnect = false;

  if (!m_isConnected) {
    return ;
  }

  stop();
  delay(10);

  ScopedLocker l(_cmd_lock);
  if (_serial) {
    if (_serial->isOpen()) {
      _serial->closePort();
    }
  }

  m_isConnected = false;
}

void YDlidarDriver::disableDataGrabbing()
{
  if (m_isScanning)
  {
    m_isScanning = false;
  }
  _dataEvent.set();
  _thread.join();
}

bool YDlidarDriver::isscanning() const {
  return m_isScanning;
}

bool YDlidarDriver::isconnected() const {
  return m_isConnected;
}

result_t YDlidarDriver::sendCommand(uint8_t cmd, const void *payload,
                                    size_t payloadsize) {
  uint8_t pkt_header[10];
  cmd_packet *header = reinterpret_cast<cmd_packet * >(pkt_header);
  uint8_t checksum = 0;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  if (payloadsize && payload) {
    cmd |= LIDAR_CMDFLAG_HAS_PAYLOAD;
  }

  header->syncByte = LIDAR_CMD_SYNC_BYTE;
  header->cmd_flag = cmd;
  sendData(pkt_header, 2) ;

  if ((cmd & LIDAR_CMDFLAG_HAS_PAYLOAD) && payloadsize && payload) {
    checksum ^= LIDAR_CMD_SYNC_BYTE;
    checksum ^= cmd;
    checksum ^= (payloadsize & 0xFF);

    for (size_t pos = 0; pos < payloadsize; ++pos) {
      checksum ^= ((uint8_t *)payload)[pos];
    }

    uint8_t sizebyte = (uint8_t)(payloadsize);
    sendData(&sizebyte, 1);

    sendData((const uint8_t *)payload, sizebyte);

    sendData(&checksum, 1);
  }

  return RESULT_OK;
}

result_t YDlidarDriver::sendData(const uint8_t *data, size_t size) {
  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  if (data == NULL || size == 0) {
    return RESULT_FAIL;
  }

  size_t r = 0;

  while (size) {
    r = _serial->writeData(data, size);

    if (r < 1) {
      return RESULT_FAIL;
    }

    // printf("send: ");
    // printHex(data, r);

    size -= r;
    data += r;
  }

  return RESULT_OK;
}

result_t YDlidarDriver::getData(uint8_t *data, size_t size) {
  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  size_t r = 0;

  while (size) {
    r = _serial->readData(data, size);

    if (r < 1) {
      return RESULT_FAIL;
    }

    // printf("recv: ");
    // printHex(data, r);

    size -= r;
    data += r;
  }

  return RESULT_OK;
}

result_t YDlidarDriver::waitResponseHeader(lidar_ans_header *header,
    uint32_t timeout) {
  int  recvPos     = 0;
  uint32_t startTs = getms();
  uint8_t  recvBuffer[sizeof(lidar_ans_header)];
  uint8_t  *headerBuffer = reinterpret_cast<uint8_t *>(header);
  uint32_t waitTime = 0;
  has_device_header = false;
  last_device_byte = 0x00;

  while ((waitTime = getms() - startTs) <= timeout) {
    size_t remainSize = sizeof(lidar_ans_header) - recvPos;
    size_t recvSize = 0;
    result_t ans = waitForData(remainSize, timeout - waitTime, &recvSize);

    if (!IS_OK(ans)) {
      return ans;
    }

    if (recvSize > remainSize) {
      recvSize = remainSize;
    }

    ans = getData(recvBuffer, recvSize);

    if (IS_FAIL(ans)) {
      return RESULT_FAIL;
    }

    for (size_t pos = 0; pos < recvSize; ++pos) {
      uint8_t currentByte = recvBuffer[pos];

      switch (recvPos) {
        case 0:
          if (currentByte != LIDAR_ANS_SYNC_BYTE1) {
            if (last_device_byte == (PH & 0xFF) && currentByte == (PH >> 8)) {
              has_device_header = true;
            }

            last_device_byte = currentByte;
            continue;
          }

          break;

        case 1:
          if (currentByte != LIDAR_ANS_SYNC_BYTE2) {
            last_device_byte = currentByte;
            recvPos = 0;
            continue;
          }

          break;
      }

      headerBuffer[recvPos++] = currentByte;
      last_device_byte = currentByte;

      if (recvPos == sizeof(lidar_ans_header)) {
        return RESULT_OK;
      }
    }
  }

  return RESULT_FAIL;
}

result_t YDlidarDriver::waitForData(size_t data_count, uint32_t timeout,
                                    size_t *returned_size) {
  size_t length = 0;

  if (returned_size == NULL) {
    returned_size = (size_t *)&length;
  }

  return (result_t)_serial->waitfordata(data_count, timeout, returned_size);
}

result_t YDlidarDriver::checkAutoConnecting(bool serialError)
{
  result_t ans = RESULT_FAIL;
  isAutoconnting = true;
  m_InvalidNodeCount = 0;

  if (m_driverErrno != BlockError)
  {
    setDriverError(TimeoutError);
  }

  while (isAutoReconnect && isAutoconnting)
  {
    {
      ScopedLocker l(_cmd_lock);
      if (_serial)
      {
        if (_serial->isOpen() || m_isConnected)
        {
            size_t buffer_size = _serial->available();
            m_BufferSize += buffer_size;

            if (m_BufferSize && m_BufferSize % 7 == 0)
            {
              setDriverError(BlockError);
            }
            else
            {
              if (buffer_size > 0 || m_BufferSize > 0)
              {
                if (m_driverErrno != BlockError)
                {
                  setDriverError(TrembleError);
                }
              }
              else
              {
                setDriverError(NotBufferError);
              }
            }

            if ((retryCount % 2 == 1) || serialError)
            {
              m_isConnected = false;
              _serial->closePort();
              delete _serial;
              _serial = NULL;
            }
            else
            {
              m_BufferSize -= buffer_size;
            }
        }
      }
    }

    if (!m_isConnected && ((retryCount % 2 == 1) || serialError))
    {
      if (!IS_OK(connect(serial_port.c_str(), m_baudrate)))
      {
        setDriverError(NotOpenError);
      }
    }

    retryCount++;

    if (retryCount > 50)
    {
      retryCount = 50;
    }

    int tempCount = 0;

    while (isAutoReconnect && isscanning() && tempCount < retryCount)
    {
      delay(200);
      tempCount++;
    }

    tempCount = 0;
    int retryConnect = 0;

    while (isAutoReconnect &&
           connect(serial_port.c_str(), m_baudrate) != RESULT_OK)
    {
      retryConnect++;

      if (retryConnect > 25)
      {
        retryConnect = 25;
      }

      tempCount = 0;
      setDriverError(NotOpenError);

      while (isAutoReconnect && isscanning() && tempCount < retryConnect)
      {
        delay(200);
        tempCount++;
      }
    }

    if (!isAutoReconnect)
    {
      m_isScanning = false;
      isAutoconnting = false;
      return RESULT_FAIL;
    }

    if (isconnected())
    {
      delay(50);

      if (!m_SingleChannel && m_driverErrno != BlockError)
      {
        device_info devinfo;
        ans = getDeviceInfo(devinfo, 1000);
        if (!IS_OK(ans))
        {
            stopScan();
            ans = getDeviceInfo(devinfo);
        }

        if (!IS_OK(ans))
        {
            setDriverError(DeviceNotFoundError);
            continue;
        }
      }

      {
        ans = startAutoScan();
        if (!IS_OK(ans))
            ans = startAutoScan();
      }

      if (IS_OK(ans))
      {
        isAutoconnting = false;

        if (getDriverError() == DeviceNotFoundError)
        {
            setDriverError(NoError);
        }

        return ans;
      }
      else
      {
        setDriverError(DeviceNotFoundError);
      }
    }
  }

  isAutoconnting = false;
  return RESULT_FAIL;
}

result_t YDlidarDriver::autoHeartBeat() {
  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  ScopedLocker l(_cmd_lock);
  result_t ans = sendCommand(LIDAR_CMD_SCAN);
  return ans;
}

void YDlidarDriver::KeepLiveHeartBeat() {
  if (m_HeartBeat) {
    uint32_t end_ts = getms();

    if (end_ts - m_heartbeat_ts > DEFAULT_HEART_BEAT) {
      autoHeartBeat();
      m_heartbeat_ts = end_ts;
    }
  }
}

void YDlidarDriver::CheckLaserStatus() {
  if (m_InvalidNodeCount < 2) {
    if (m_driverErrno == NoError) {
      setDriverError(LaserFailureError);
    }
  } else {
    if (m_driverErrno == LaserFailureError) {
      setDriverError(NoError);
    }
  }

  m_InvalidNodeCount = 0;
}

//解析扫描数据线程主函数
int YDlidarDriver::cacheScanData()
{
    node_info      local_buf[128];
    size_t         count = 128;
    node_info      local_scan[MAX_SCAN_NODES];
    size_t         scan_count = 0;
    result_t       ans = RESULT_FAIL;
    memset(local_scan, 0, sizeof(local_scan));

    int timeout_count = 0;
    retryCount = 0;
    m_BufferSize = 0;
    m_heartbeat_ts = getms();
    bool m_last_frame_valid = false;

    m_isScanning = true;

    while (m_isScanning)
    {
        count = 128;
        ans = waitScanData(local_buf, count, DEFAULT_TIMEOUT / 2);

        Thread::needExit();

        if (!IS_OK(ans)) {
            if (IS_FAIL(ans) || timeout_count > DEFAULT_TIMEOUT_COUNT) {
                if (!isAutoReconnect) {
                    fprintf(stderr, "exit scanning thread!!\n");
                    fflush(stderr);
                    {
                        m_isScanning = false;
                    }
                    return RESULT_FAIL;
                } else {
                    if (m_last_frame_valid) {
                        m_BufferSize = 0;
                        m_last_frame_valid = false;
                    }

                    ans = checkAutoConnecting(IS_FAIL(ans));
                    if (IS_OK(ans)) {
                        timeout_count = 0;
                        local_scan[0].sync = Node_NotSync;
                    } else {
                        m_isScanning = false;
                        return RESULT_FAIL;
                    }
                }
            } else {
                timeout_count++;
                local_scan[0].sync = Node_NotSync;

                if (m_driverErrno == NoError) {
                    setDriverError(TimeoutError);
                }

                fprintf(stderr, "timeout count: %d\n", timeout_count);
                fflush(stderr);
            }
        } else {
            timeout_count = 0;
            m_last_frame_valid = true;

            if (retryCount != 0) {
                setDriverError(NoError);
            }

            retryCount = 0;
            m_BufferSize = 0;
        }

        for (size_t pos = 0; pos < count; ++pos)
        {
            if (local_buf[pos].sync & LIDAR_RESP_SYNCBIT)
            {
                // printf("[YDLIDAR] S2 points Stored in buffer start %lu\n", scan_count);
                if (local_scan[0].sync & LIDAR_RESP_SYNCBIT)
                {
                    ScopedLocker l(_lock);
                    //将下一圈的第一个点的采集时间作为当前圈数据的采集时间
                    local_scan[0].delayTime = local_buf[pos].delayTime;
                    memcpy(scan_node_buf, local_scan, scan_count * sizeof(node_info));
                    scan_node_count = scan_count;
                    _dataEvent.set();
                    // printf("[YDLIDAR] S2 points Stored in buffer end %lu\n", scan_count);
                }

                scan_count = 0;
            }

            local_scan[scan_count++] = local_buf[pos];

            if (scan_count == _countof(local_scan)) {
                scan_count -= 1;
            }
        }

        KeepLiveHeartBeat();
    }

    m_isScanning = false;

    return RESULT_OK;
}

result_t YDlidarDriver::checkDeviceInfo(uint8_t *recvBuffer, uint8_t byte,
                                        int recvPos, int recvSize, int pos) {
  if (asyncRecvPos == sizeof(lidar_ans_header)) {
    if ((((pos < recvSize - 1) && byte == LIDAR_ANS_SYNC_BYTE1) ||
         (last_device_byte == LIDAR_ANS_SYNC_BYTE1 && byte == LIDAR_ANS_SYNC_BYTE2)) &&
        recvPos == 0) {
      if ((last_device_byte == LIDAR_ANS_SYNC_BYTE1 &&
           byte == LIDAR_ANS_SYNC_BYTE2)) {
        asyncRecvPos = 0;
        async_size = 0;
        headerBuffer[asyncRecvPos] = last_device_byte;
        asyncRecvPos++;
        headerBuffer[asyncRecvPos] = byte;
        asyncRecvPos++;
        last_device_byte = byte;
        return RESULT_OK;
      } else {
        if (pos < recvSize - 1) {
          if (recvBuffer[pos + 1] == LIDAR_ANS_SYNC_BYTE2) {
            asyncRecvPos = 0;
            async_size = 0;
            headerBuffer[asyncRecvPos] = byte;
            asyncRecvPos++;
            last_device_byte = byte;
            return RESULT_OK;
          }
        }

      }

    }

    last_device_byte = byte;

    if (header_.type == LIDAR_ANS_TYPE_DEVINFO ||
        header_.type == LIDAR_ANS_TYPE_DEVHEALTH) {
      if (header_.size < 1) {
        asyncRecvPos = 0;
        async_size = 0;
      } else {

        if (header_.type == LIDAR_ANS_TYPE_DEVHEALTH) {
          if (async_size < sizeof(health_)) {
            healthBuffer[async_size] = byte;
            async_size++;

            if (async_size == sizeof(health_)) {
              asyncRecvPos = 0;
              async_size = 0;
              get_device_health_success = true;
              last_device_byte = byte;
              return RESULT_OK;
            }

          } else {
            asyncRecvPos = 0;
            async_size = 0;
          }

        } else {

          if (async_size < sizeof(info_)) {
            infoBuffer[async_size] = byte;
            async_size++;

            if (async_size == sizeof(info_)) {
              asyncRecvPos = 0;
              async_size = 0;
              m_HasDeviceInfo = true;

              last_device_byte = byte;
              return RESULT_OK;
            }

          } else {
            asyncRecvPos = 0;
            async_size = 0;
          }
        }
      }
    } else if (header_.type == LIDAR_ANS_TYPE_MEASUREMENT) {
      asyncRecvPos = 0;
      async_size = 0;

    }

  } else {

    switch (asyncRecvPos) {
      case 0:
        if (byte == LIDAR_ANS_SYNC_BYTE1 && recvPos == 0) {
          headerBuffer[asyncRecvPos] = byte;
          last_device_byte = byte;
          asyncRecvPos++;
        }

        break;

      case 1:
        if (byte == LIDAR_ANS_SYNC_BYTE2 && recvPos == 0) {
          headerBuffer[asyncRecvPos] = byte;
          asyncRecvPos++;
          last_device_byte = byte;
          return RESULT_OK;
        } else {
          asyncRecvPos = 0;
        }

        break;

      default:
        break;
    }

    if (asyncRecvPos >= 2) {
      if (((pos < recvSize - 1 && byte == LIDAR_ANS_SYNC_BYTE1) ||
           (last_device_byte == LIDAR_ANS_SYNC_BYTE1 && byte == LIDAR_ANS_SYNC_BYTE2)) &&
          recvPos == 0) {
        if ((last_device_byte == LIDAR_ANS_SYNC_BYTE1 &&
             byte == LIDAR_ANS_SYNC_BYTE2)) {
          asyncRecvPos = 0;
          async_size = 0;
          headerBuffer[asyncRecvPos] = last_device_byte;
          asyncRecvPos++;
        } else {
          if (pos < recvSize - 2) {
            if (recvBuffer[pos + 1] == LIDAR_ANS_SYNC_BYTE2) {
              asyncRecvPos = 0;
            }
          }
        }
      }

      headerBuffer[asyncRecvPos] = byte;
      asyncRecvPos++;
      last_device_byte = byte;
      return RESULT_OK;
    }
  }

  return RESULT_FAIL;

}

result_t YDlidarDriver::waitDevicePackage(uint32_t timeout) {
  int recvPos = 0;
  asyncRecvPos = 0;
  uint32_t startTs = getms();
  uint32_t waitTime = 0;
  result_t ans = RESULT_FAIL;

  while ((waitTime = getms() - startTs) <= timeout) {
    size_t remainSize = PackagePaidBytes - recvPos;
    size_t recvSize = 0;
    result_t ans = waitForData(remainSize, timeout - waitTime, &recvSize);

    if (!IS_OK(ans)) {
      return ans;
    }

    ans = RESULT_FAIL;

    if (recvSize > remainSize) {
      recvSize = remainSize;
    }

    getData(globalRecvBuffer, recvSize);

    for (size_t pos = 0; pos < recvSize; ++pos) {
      uint8_t currentByte = globalRecvBuffer[pos];

      if (checkDeviceInfo(globalRecvBuffer, currentByte, recvPos, recvSize,
                          pos) == RESULT_OK) {
        continue;
      }
    }

    if (m_HasDeviceInfo) {
      ans = RESULT_OK;
      break;
    }
  }

  flushSerial();
  return ans;
}

void YDlidarDriver::checkBlockStatus(uint8_t currentByte) {
  switch (m_BlockRevSize) {
    case 0:
      if (currentByte == LIDAR_ANS_SYNC_BYTE1) {
        m_BlockRevSize++;
      }

      break;

    case 1:
      if (currentByte == LIDAR_ANS_SYNC_BYTE2) {
        setDriverError(BlockError);
        m_BlockRevSize = 0;
      }

      break;

    default:
      break;
  }
}

result_t YDlidarDriver::parseResponseHeader(
    uint8_t *packageBuffer,
    uint32_t timeout)
{
  int recvPos = 0;
  uint32_t startTs = getms();
  uint32_t waitTime = 0;
  m_BlockRevSize = 0;
  package_Sample_Num = 0;
  uint8_t package_type = 0;
  result_t ans = RESULT_TIMEOUT;

  while ((waitTime = getms() - startTs) <= timeout)
  {
    size_t remainSize = PackagePaidBytes - recvPos;
    size_t recvSize = 0;
    ans = waitForData(remainSize, timeout - waitTime, &recvSize);
    if (!IS_OK(ans))
    {
      return ans;
    }

    if (recvSize > remainSize)
      recvSize = remainSize;

    ans = getData(globalRecvBuffer, recvSize);

    for (size_t pos = 0; pos < recvSize; ++pos)
    {
      uint8_t currentByte = globalRecvBuffer[pos];

      // printf("c:%02X p:%d\n", currentByte, recvPos);

      switch (recvPos)
      {
      case 0:
        if (currentByte != PH1)
        {
          checkBlockStatus(currentByte);
          continue;
        }
        break;

      case 1:
      {
        CheckSumCal = PH;
        if (currentByte == PH2)
        {
          if (m_driverErrno == BlockError)
          {
            setDriverError(NoError);
          }
        }
        else if (currentByte == PH1) //防止出现连续0xAA
        {
          continue;
        }
        else if (currentByte == PH3) //时间戳标识
        {
          recvPos = 0;
          size_t lastPos = pos - 1;
          //解析时间戳（共8个字节）
          int remainSize = SIZE_STAMPPACKAGE - (recvSize - pos + 1); //计算剩余应读字节数
          if (remainSize > 0)
          {
            // printf("remainSize: %u\n", remainSize);
            size_t lastSize = recvSize;
            ans = waitForData(remainSize, timeout - waitTime, &recvSize);
            if (!IS_OK(ans))
              return ans;
            if (recvSize > remainSize)
              recvSize = remainSize;
            getData(&globalRecvBuffer[lastSize], recvSize);
            recvSize += lastSize;
            pos = PackagePaidBytes;
          }
          else
          {
            pos += 6;
          }

          //校验和检测
          uint8_t csc = 0; //计算校验和
          uint8_t csr = 0; //实际校验和
          for (int i=0; i<SIZE_STAMPPACKAGE; ++i)
          {
            if (i == 2)
              csr = globalRecvBuffer[lastPos + i];
            else
              csc ^= globalRecvBuffer[lastPos + i];
          }
          if (csc != csr)
          {
            printf("[YDLIDAR] Checksum error c[0x%02X] != r[0x%02X]\n", csc, csr);
          }
          else
          {
            stamp_package sp;
            memcpy(&sp, &globalRecvBuffer[lastPos], SIZE_STAMPPACKAGE);
            stamp = uint64_t(sp.stamp) * 1000000; //毫秒转纳秒需要×1000000
            // printf("stamp: 0x%"PRIx64" -> 0x%"PRIx64"\n", sp.stamp, stamp);
            fflush(stdout);
          }

          continue;
        }
        else
        {
          has_package_error = true;
          recvPos = 0;
          continue;
        }
        break;
      }
      case 2:
        SampleNumlAndCTCal = currentByte;
        package_type = currentByte & 0x01; //是否是零位包标识

        if ((package_type == CT_Normal) || 
          (package_type == CT_RingStart))
        {
          if (package_type == CT_RingStart)
          {
            scan_frequence = (currentByte & 0xFE) >> 1;
          }
        }
        else
        {
          has_package_error = true;
          recvPos = 0;
          continue;
        }
        break;

      case 3:
        SampleNumlAndCTCal += (currentByte * 0x100);
        package_Sample_Num = currentByte;
        break;

      case 4:
        if (currentByte & LIDAR_RESP_CHECKBIT)
        {
          FirstSampleAngle = currentByte;
        }
        else
        {
          has_package_error = true;
          recvPos = 0;
          continue;
        }
        break;

      case 5:
        FirstSampleAngle += currentByte * 0x100;
        CheckSumCal ^= FirstSampleAngle;
        FirstSampleAngle = FirstSampleAngle >> 1;
        break;

      case 6:
        if (currentByte & LIDAR_RESP_CHECKBIT)
        {
          LastSampleAngle = currentByte;
        }
        else
        {
          has_package_error = true;
          recvPos = 0;
          continue;
        }

        break;

      case 7:
        LastSampleAngle = currentByte * 0x100 + LastSampleAngle;
        LastSampleAngleCal = LastSampleAngle;
        LastSampleAngle = LastSampleAngle >> 1;

        if (package_Sample_Num == 1)
        {
          IntervalSampleAngle = 0;
        }
        else
        {
          if (LastSampleAngle < FirstSampleAngle)
          {
            if ((FirstSampleAngle > 270 * 64) && (LastSampleAngle < 90 * 64))
            {
              IntervalSampleAngle = (float)((360 * 64 + LastSampleAngle -
                                             FirstSampleAngle) /
                                            ((
                                                 package_Sample_Num - 1) *
                                             1.0));
              IntervalSampleAngle_LastPackage = IntervalSampleAngle;
            }
            else
            {
              IntervalSampleAngle = IntervalSampleAngle_LastPackage;
            }
          }
          else
          {
            IntervalSampleAngle = (float)((LastSampleAngle - FirstSampleAngle) / ((
                                                                                      package_Sample_Num - 1) *
                                                                                  1.0));
            IntervalSampleAngle_LastPackage = IntervalSampleAngle;
          }
        }

        break;

      case 8:
        CheckSum = currentByte;
        break;

      case 9:
        CheckSum += (currentByte * 0x100);
        break;
      }

      packageBuffer[recvPos++] = currentByte;
    }

    if (recvPos == PackagePaidBytes)
    {
      ans = RESULT_OK;
      break;
    }

    ans = RESULT_TIMEOUT;
  }

  return ans;
}

result_t YDlidarDriver::parseResponseScanData(
    uint8_t *packageBuffer,
    uint32_t timeout)
{
  int recvPos = 0;
  uint32_t startTs = getms();
  uint32_t waitTime = 0;
  result_t ans = RESULT_TIMEOUT;

  while ((waitTime = getms() - startTs) <= timeout)
  {
    size_t remainSize = package_Sample_Num * PackageSampleBytes - recvPos;
    size_t recvSize = 0;
    ans = waitForData(remainSize, timeout - waitTime, &recvSize);

    if (!IS_OK(ans))
      return ans;

    if (recvSize > remainSize)
      recvSize = remainSize;

    getData(globalRecvBuffer, recvSize);

    for (size_t pos = 0; pos < recvSize; ++pos)
    {
      if (m_intensities && !isTOFLidar(m_LidarType))
      {
        if (recvPos % 3 == 2)
        {
          Valu8Tou16 += globalRecvBuffer[pos] * 0x100;
          CheckSumCal ^= Valu8Tou16;
        }
        else if (recvPos % 3 == 1)
        {
          Valu8Tou16 = globalRecvBuffer[pos];
        }
        else
        {
          CheckSumCal ^= globalRecvBuffer[pos];
        }
      }
      else
      {
        if (recvPos % 2 == 1)
        {
          Valu8Tou16 += globalRecvBuffer[pos] * 0x100;
          CheckSumCal ^= Valu8Tou16;
        }
        else
        {
          Valu8Tou16 = globalRecvBuffer[pos];
        }
      }

      packageBuffer[PackagePaidBytes + recvPos] = globalRecvBuffer[pos];
      recvPos++;
    }

    if (package_Sample_Num * PackageSampleBytes == recvPos)
    {
      ans = RESULT_OK;
      break;
    }
  }

  if (package_Sample_Num * PackageSampleBytes != recvPos)
  {
    return RESULT_FAIL;
  }

  return ans;
}

result_t YDlidarDriver::waitPackage(node_info *node, uint32_t timeout) 
{
  (*node).index = 255;
  (*node).scanFreq  = 0;
  (*node).error = 0;
  (*node).debugInfo = 0xff;

  if (package_Sample_Index == 0) 
  {
    uint8_t  *packageBuffer = (m_intensities) ? (isTOFLidar(m_LidarType) ?
                              (uint8_t *)&tof_package.head : 
                              (uint8_t *)&package.head) :
                              (uint8_t *)&packages.head;
    result_t ans = parseResponseHeader(packageBuffer, timeout);
    if (!IS_OK(ans)) {
      return ans;
    }
    // printf("index %u\n", package_Sample_Index);

    ans = parseResponseScanData(packageBuffer, timeout);
    if (!IS_OK(ans)) {
      return ans;
    }

    calcuteCheckSum(node);
    calcutePackageCT();
  }

  parseNodeDebugFromBuffer(node);
  parseNodeFromeBuffer(node);
  return RESULT_OK;
}

void YDlidarDriver::calcuteCheckSum(node_info *node) {
  CheckSumCal ^= SampleNumlAndCTCal;
  CheckSumCal ^= LastSampleAngleCal;

  if (CheckSumCal != CheckSum) {
    CheckSumResult = false;
    has_package_error = true;
    (*node).error = 1;
  } else {
    CheckSumResult = true;
  }
}

void YDlidarDriver::calcutePackageCT() {
  if (m_intensities) {
    if (isTOFLidar(m_LidarType)) {
      ct = tof_package.ct;
      nowPackageNum = tof_package.count;
    } else {
      ct = package.ct;
      nowPackageNum = package.count;
    }
  } else {
    ct = packages.ct;
    nowPackageNum = packages.count;
  }
  // printf("[YDLIDAR] S2 pack points %u\n", nowPackageNum);
}

void YDlidarDriver::parseNodeDebugFromBuffer(node_info *node)
{
    if ((ct & 0x01) == CT_Normal) {
        (*node).sync = Node_NotSync;
        (*node).debugInfo = 0xff;

        if (!has_package_error) {
            if (package_Sample_Index == 0) {
                package_index++;
                (*node).debugInfo = (ct >> 1);
                (*node).index = package_index;
            }
        } else {
            (*node).error = 1;
            (*node).index = 255;
            package_index = 0xff;
        }
    } else {
        (*node).sync = Node_Sync;
        package_index = 0;

        // printf("start angle %f end angle %f\n", 
        // float(FirstSampleAngle) / 64.0,
        // float(LastSampleAngle) / 64.0);

        if (CheckSumResult) {
            has_package_error = false;
            (*node).index = package_index;
            (*node).debugInfo = (ct >> 1);
            (*node).scanFreq = scan_frequence;
        }
    }
}

void YDlidarDriver::parseNodeFromeBuffer(node_info *node)
{
    int32_t correctAngle = 0;
    (*node).qual = Node_Default_Quality;
    (*node).delayTime = 0;
    (*node).stamp = stamp ? stamp : getTime();
    (*node).scanFreq = scan_frequence;
    (*node).is = 0;

    if (CheckSumResult)
    {
        if (m_intensities) //如果带强度信息
        {
            if (isTriangleLidar(m_LidarType))
            {
                if (8 == m_intensityBit)
                {
                    (*node).qual = uint16_t(package.nodes[package_Sample_Index].qual);
                }
                else
                {
                    (*node).qual = ((uint16_t)((package.nodes[package_Sample_Index].dist & 0x03) <<
                        LIDAR_RESP_ANGLE_SAMPLE_SHIFT) |
                        (package.nodes[package_Sample_Index].qual));
                }

                (*node).dist =
                        package.nodes[package_Sample_Index].dist & 0xfffc;
                (*node).is = package.nodes[package_Sample_Index].dist & 0x0003;

                // printf("%d i %u\n", package_Sample_Index, (*node).qual);
                // fflush(stdout);
            }
            else
            {
                (*node).qual =
                        tof_package.nodes[package_Sample_Index].qual;
                (*node).dist =
                        tof_package.nodes[package_Sample_Index].dist;
            }
        }
        else //如果不带强度信息
        {
            (*node).dist = packages.nodes[package_Sample_Index];

            if (isTriangleLidar(m_LidarType)) 
            {
                (*node).qual = ((uint16_t)(0xfc |
                    packages.nodes[package_Sample_Index] &
                    0x0003)) << LIDAR_RESP_QUALITY_SHIFT;
            }
        }

        if ((*node).dist != 0)
        {
            if (isOctaveLidar(model))
            {
                correctAngle = (int32_t)(((atan(((21.8 * (155.3 - ((*node).dist / 2.0))) / 155.3) / ((*node).dist / 2.0))) * 180.0 / 3.1415) * 64.0);
            }
            else if (isSCLLidar(model))
            {
                //SCL雷达角度二级解析公式（α = asind（17.8/dist））
                correctAngle = int32_t(asin(17.8 / node->dist) * 180.0 / M_PI * 64.0);
                printf("SCL correct angle [%d]\n", correctAngle);
            }
            else if (isTriangleLidar(m_LidarType) &&
                !isTminiLidar(model)) //去掉Tmini雷达的角度二级解析
            {
//                printf("has angle 2nd parse\n");
                correctAngle = (int32_t)(((atan(((21.8 * (155.3 - ((*node).dist / 4.0))) / 155.3) / ((*node).dist / 4.0))) * 180.0 / 3.1415) * 64.0);
            }
            else
            {
//                printf("no angle 2nd parse\n");
            }

            m_InvalidNodeCount ++;
        }
        else
        {
            correctAngle = 0;
        }

        float sampleAngle = IntervalSampleAngle * package_Sample_Index;

        if ((FirstSampleAngle + sampleAngle +
             correctAngle) < 0) {
            (*node).angle = (((uint16_t)(FirstSampleAngle + sampleAngle +
                                                     correctAngle + 23040)) << LIDAR_RESP_ANGLE_SHIFT) +
                    LIDAR_RESP_CHECKBIT;
        } else {
            if ((FirstSampleAngle + sampleAngle + correctAngle) > 23040) {
                (*node).angle = (((uint16_t)(FirstSampleAngle + sampleAngle +
                                                         correctAngle - 23040)) << LIDAR_RESP_ANGLE_SHIFT) +
                        LIDAR_RESP_CHECKBIT;
            } else {
                (*node).angle = (((uint16_t)(FirstSampleAngle + sampleAngle +
                                                         correctAngle)) << LIDAR_RESP_ANGLE_SHIFT) +
                        LIDAR_RESP_CHECKBIT;
            }
        }
    } else {
        (*node).sync = Node_NotSync;
        (*node).qual = Node_Default_Quality;
        (*node).angle = LIDAR_RESP_CHECKBIT;
        (*node).dist = 0;
        (*node).scanFreq = 0;
    }

    package_Sample_Index ++;

    if (package_Sample_Index >= nowPackageNum) 
    {
        package_Sample_Index = 0;
        CheckSumResult = false;
    }
}

result_t YDlidarDriver::waitScanData(
        node_info *nodebuffer,
        size_t &count,
        uint32_t timeout)
{
    if (!m_isConnected) {
        count = 0;
        return RESULT_FAIL;
    }

    size_t     recvNodeCount    =  0;
    uint32_t   startTs          = getms();
    uint32_t   waitTime         = 0;
    result_t   ans              = RESULT_FAIL;

    while ((waitTime = getms() - startTs) <= timeout &&
           recvNodeCount < count)
    {
        node_info node;
        ans = waitPackage(&node, timeout - waitTime);
        if (!IS_OK(ans)) {
            count = recvNodeCount;
            return ans;
        }

        nodebuffer[recvNodeCount++] = node;

        //如果是零位包点
        if (node.sync & LIDAR_RESP_SYNCBIT)
        {
            //计算延时时间
            size_t size = _serial->available();
            uint64_t delayTime = 0;
            if (size > PackagePaidBytes) {
                size_t packageNum = 0;
                size_t Number = 0;
                size_t PackageSize = TrianglePackageDataSize;
                packageNum = size / PackageSize;
                Number = size % PackageSize;
                delayTime = packageNum * (PackageSize - PackagePaidBytes) * m_PointTime / 2;

                if (Number > PackagePaidBytes) {
                    delayTime += m_PointTime * ((Number - PackagePaidBytes) / 2);
                }
            }
            nodebuffer[recvNodeCount - 1].delayTime = size * trans_delay + delayTime;

//            nodebuffer[recvNodeCount - 1].scanFreq = node.scanFreq;
//            nodebuffer[recvNodeCount - 1].stamp = getTime();
            count = recvNodeCount;
            CheckLaserStatus();
            return RESULT_OK;
        }

        if (recvNodeCount == count) {
            return RESULT_OK;
        }
    }

    count = recvNodeCount;
    return RESULT_FAIL;
}

result_t YDlidarDriver::grabScanData(node_info *nodebuffer,
                                     size_t &count,
                                     uint32_t timeout)
{
    switch (_dataEvent.wait(timeout))
    {
    case Event::EVENT_TIMEOUT:
        count = 0;
        return RESULT_TIMEOUT;

    case Event::EVENT_OK:
    {
        ScopedLocker l(_lock);
        // if (scan_node_count == 0)
        // {
        //     return RESULT_FAIL;
        // }

        size_t size_to_copy = min(count, scan_node_count);
        memcpy(nodebuffer, scan_node_buf, size_to_copy * sizeof(node_info));
        count = size_to_copy;
        scan_node_count = 0;
    }
        return RESULT_OK;

    default:
        count = 0;
        return RESULT_FAIL;
    }
}

result_t YDlidarDriver::ascendScanData(node_info *nodebuffer, size_t count) {
  float inc_origin_angle = (float)360.0 / count;
  int i = 0;

  for (i = 0; i < (int)count; i++) {
    if (nodebuffer[i].dist == 0) {
      continue;
    } else {
      while (i != 0) {
        i--;
        float expect_angle = (nodebuffer[i + 1].angle >>
                              LIDAR_RESP_ANGLE_SHIFT) /
                             64.0f - inc_origin_angle;

        if (expect_angle < 0.0f) {
          expect_angle = 0.0f;
        }

        uint16_t checkbit = nodebuffer[i].angle &
                            LIDAR_RESP_CHECKBIT;
        nodebuffer[i].angle = (((uint16_t)(expect_angle * 64.0f)) <<
                                           LIDAR_RESP_ANGLE_SHIFT) + checkbit;
      }

      break;
    }
  }

  if (i == (int)count) {
    return RESULT_FAIL;
  }

  for (i = (int)count - 1; i >= 0; i--) {
    if (nodebuffer[i].dist == 0) {
      continue;
    } else {
      while (i != ((int)count - 1)) {
        i++;
        float expect_angle = (nodebuffer[i - 1].angle >>
                              LIDAR_RESP_ANGLE_SHIFT) /
                             64.0f + inc_origin_angle;

        if (expect_angle > 360.0f) {
          expect_angle -= 360.0f;
        }

        uint16_t checkbit = nodebuffer[i].angle &
                            LIDAR_RESP_CHECKBIT;
        nodebuffer[i].angle = (((uint16_t)(expect_angle * 64.0f)) <<
                                           LIDAR_RESP_ANGLE_SHIFT) + checkbit;
      }

      break;
    }
  }

  float frontAngle = (nodebuffer[0].angle >>
                      LIDAR_RESP_ANGLE_SHIFT) / 64.0f;

  for (i = 1; i < (int)count; i++) {
    if (nodebuffer[i].dist == 0) {
      float expect_angle =  frontAngle + i * inc_origin_angle;

      if (expect_angle > 360.0f) {
        expect_angle -= 360.0f;
      }

      uint16_t checkbit = nodebuffer[i].angle &
                          LIDAR_RESP_CHECKBIT;
      nodebuffer[i].angle = (((uint16_t)(expect_angle * 64.0f)) <<
                                         LIDAR_RESP_ANGLE_SHIFT) + checkbit;
    }
  }

  size_t zero_pos = 0;
  float pre_degree = (nodebuffer[0].angle >>
                      LIDAR_RESP_ANGLE_SHIFT) / 64.0f;

  for (i = 1; i < (int)count ; ++i) {
    float degree = (nodebuffer[i].angle >>
                    LIDAR_RESP_ANGLE_SHIFT) / 64.0f;

    if (zero_pos == 0 && (pre_degree - degree > 180)) {
      zero_pos = i;
      break;
    }

    pre_degree = degree;
  }

  node_info *tmpbuffer = new node_info[count];

  for (i = (int)zero_pos; i < (int)count; i++) {
    tmpbuffer[i - zero_pos] = nodebuffer[i];
  }

  for (i = 0; i < (int)zero_pos; i++) {
    tmpbuffer[i + (int)count - zero_pos] = nodebuffer[i];
  }

  memcpy(nodebuffer, tmpbuffer, count * sizeof(node_info));
  delete[] tmpbuffer;

  return RESULT_OK;
}

/************************************************************************/
/* get health state of lidar                                            */
/************************************************************************/
result_t YDlidarDriver::getHealth(device_health &health, uint32_t timeout) {
  result_t ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  if (m_SingleChannel) {
    if (get_device_health_success) {
      health = this->health_;
      return RESULT_OK;
    }

    health.error_code = 0;
    health.status = 0;
    return RESULT_OK;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_GET_DEVICE_HEALTH)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVHEALTH) {
      return RESULT_FAIL;
    }

    if (response_header.size < sizeof(device_health)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&health), sizeof(health));
  }
  return RESULT_OK;
}

/************************************************************************/
/* get device info of lidar                                             */
/************************************************************************/
result_t YDlidarDriver::getDeviceInfo(device_info &info, uint32_t timeout) 
{
  result_t ans = RESULT_FAIL;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  //单通，仅能获取到模组设备信息
  if (m_SingleChannel)
  {
    //获取启动时抛出的设备信息或每帧数据中的设备信息
    if (m_HasDeviceInfo)
    {
      info = this->info_;
      return RESULT_OK;
    }
    else
    {
      //未获取到设备信息时，返回一个无效的设备信息
      info.model = YDLIDAR_S2;
      return RESULT_OK;
    }
  }
  //双通，此处仅能获取到底板设备信息
  else
  {
    if (m_Bottom)
    {
      flushSerial();
      ScopedLocker l(_cmd_lock);
      if ((ans = sendCommand(LIDAR_CMD_GET_DEVICE_INFO)) != RESULT_OK)
        return ans;

      lidar_ans_header response_header;
      if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK)
        return ans;
      if (response_header.type != LIDAR_ANS_TYPE_DEVINFO)
        return RESULT_FAIL;
      if (response_header.size < sizeof(device_info))
        return RESULT_FAIL;
      if (waitForData(response_header.size, timeout) != RESULT_OK)
        return RESULT_FAIL;

      getData(reinterpret_cast<uint8_t *>(&info), sizeof(info));
      model = info.model;
      m_HasDeviceInfo = true;
      info_ = info;
    }
    return ans;
  }
}

bool YDlidarDriver::getDeviceInfoEx(device_info &info)
{
  if (m_HasDeviceInfo)
  {
    info = info_;
    return true;
  }

  return false;
}

/************************************************************************/
/* the set to signal quality                                            */
/************************************************************************/
void YDlidarDriver::setIntensities(const bool &isintensities) 
{
  if (m_intensities != isintensities) 
  {
    if (globalRecvBuffer) {
      delete[] globalRecvBuffer;
      globalRecvBuffer = NULL;
    }

    if (isintensities && isTOFLidar(m_LidarType)) {
      globalRecvBuffer = new uint8_t[sizeof(tof_node_package)];
    } else {
      globalRecvBuffer = new uint8_t[isintensities ? 
        sizeof(tri_node_package2) : sizeof(tri_node_package)];
    }
  }

  m_intensities = isintensities;

  if (m_intensities) {
    if (isTOFLidar(m_LidarType)) {
      PackageSampleBytes = 4;
    } else {
      PackageSampleBytes = 3;
    }
  } else {
    PackageSampleBytes = 2;
  }
}
/**
* @brief 设置雷达异常自动重新连接 \n
* @param[in] enable    是否开启自动重连:
*     true	开启
*	  false 关闭
*/
void YDlidarDriver::setAutoReconnect(const bool &enable) {
  isAutoReconnect = enable;
}


void YDlidarDriver::checkTransDelay() {
  //calc stamp
  trans_delay = _serial->getByteTime();
  sample_rate = getDefaultSampleRate(model).front() * 1000;

  switch (model) {
    case YDLIDAR_G4://g4
    case YDLIDAR_G5:
    case YDLIDAR_G4PRO:
    case YDLIDAR_F4PRO:
    case YDLIDAR_G6://g6
    case YDLIDAR_G7:
    case YDLIDAR_TG15:
    case YDLIDAR_TG30:
    case YDLIDAR_TG50:
      if (m_sampling_rate == -1) {
        sampling_rate _rate;
        _rate.rate = 0;
        getSamplingRate(_rate);
        m_sampling_rate = _rate.rate;
      }

      sample_rate =  ConvertLidarToUserSmaple(model, m_sampling_rate);
      sample_rate *= 1000;
      break;

    case YDLIDAR_G2C:
      sample_rate = 4000;
      break;

    case YDLIDAR_G1:
      sample_rate = 9000;
      break;

    case YDLIDAR_G4C:
      sample_rate = 4000;
      break;

    default:
      break;
  }

  m_PointTime = 1e9 / sample_rate;
}

/************************************************************************/
/*  start to scan                                                       */
/************************************************************************/
result_t YDlidarDriver::startScan(bool force, uint32_t timeout)
{
  result_t ans;

  if (!m_isConnected)
  {
    return RESULT_FAIL;
  }

  if (m_isScanning)
  {
    return RESULT_OK;
  }

  stop();
  checkTransDelay();
  flushSerial();
  delay(30);
  {
    ScopedLocker l(_cmd_lock);
    if ((ans = sendCommand(force ? LIDAR_CMD_FORCE_SCAN : LIDAR_CMD_SCAN)) !=
        RESULT_OK)
    {
      return ans;
    }

    if (!m_SingleChannel)
    {
      lidar_ans_header response_header;
      if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK)
      {
        return ans;
      }
      if (response_header.type != LIDAR_ANS_TYPE_MEASUREMENT)
      {
        return RESULT_FAIL;
      }
      if (response_header.size < 5)
      {
        return RESULT_FAIL;
      }
    }

    //此处仅获取模组设备信息
    if (!m_Bottom) {
        waitDevicePackage(1000);
    }
    //非Tmini系列雷达才自动获取强度标识
    if (!isTminiLidar(model))
    {
      // 获取强度标识
      getIntensityFlag();
    }

    //创建数据解析线程
    ans = createThread();
  }

  if (isSupportMotorCtrl(model))
    startMotor();

  return ans;
}

result_t YDlidarDriver::stopScan(uint32_t timeout) {
  UNUSED(timeout);

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  ScopedLocker l(_cmd_lock);
  sendCommand(LIDAR_CMD_FORCE_STOP);
  delay(5);
  sendCommand(LIDAR_CMD_STOP);
  delay(5);
  return RESULT_OK;
}

result_t YDlidarDriver::createThread() 
{
  //如果线程已启动，则先退出线程
  if (_thread.getHandle())
  {
    m_isScanning = false;
    _thread.join();
  }

  _thread = CLASS_THREAD(YDlidarDriver, cacheScanData);
  if (!_thread.getHandle()) {
    return RESULT_FAIL;
  }

  printf("[YDLIDAR] Create thread 0x%X\n", _thread.getHandle());
  fflush(stdout);

  return RESULT_OK;
}


result_t YDlidarDriver::startAutoScan(bool force, uint32_t timeout) {
  result_t ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  flushSerial();
  delay(10);
  {

    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(force ? LIDAR_CMD_FORCE_SCAN : LIDAR_CMD_SCAN)) !=
        RESULT_OK) {
      return ans;
    }

    if (!m_SingleChannel) {
      lidar_ans_header response_header;

      if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
        return ans;
      }

      if (response_header.type != LIDAR_ANS_TYPE_MEASUREMENT) {
        return RESULT_FAIL;
      }

      if (response_header.size < 5) {
        return RESULT_FAIL;
      }
    }

  }

  if (isSupportMotorCtrl(model)) {
    startMotor();
  }

  return RESULT_OK;
}

/************************************************************************/
/*   stop scan                                                   */
/************************************************************************/
result_t YDlidarDriver::stop() {
  if (isAutoconnting) {
    isAutoReconnect = false;
    m_isScanning = false;
  }

  disableDataGrabbing();
  stopScan();

  if (isSupportMotorCtrl(model)) {
    stopMotor();
  }

  return RESULT_OK;
}

/************************************************************************/
/*  reset device                                                        */
/************************************************************************/
result_t YDlidarDriver::reset(uint32_t timeout) {
  UNUSED(timeout);
  result_t ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  ScopedLocker l(_cmd_lock);

  if ((ans = sendCommand(LIDAR_CMD_RESET)) != RESULT_OK) {
    return ans;
  }

  return RESULT_OK;
}

/************************************************************************/
/*  startMotor                                                          */
/************************************************************************/
result_t YDlidarDriver::startMotor() {
  ScopedLocker l(_cmd_lock);

  if (m_SupportMotorDtrCtrl) {
    setDTR();
    delay(500);
  } else {
    clearDTR();
    delay(500);
  }

  return RESULT_OK;
}

/************************************************************************/
/*  stopMotor                                                           */
/************************************************************************/
result_t YDlidarDriver::stopMotor() {
  ScopedLocker l(_cmd_lock);

  if (m_SupportMotorDtrCtrl) {
    clearDTR();
    delay(500);
  } else {
    setDTR();
    delay(500);
  }

  return RESULT_OK;
}

/************************************************************************/
/* get the current scan frequency of lidar                              */
/************************************************************************/
result_t YDlidarDriver::getScanFrequency(scan_frequency &frequency,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_GET_AIMSPEED)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(frequency)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&frequency), sizeof(frequency));
  }
  return RESULT_OK;
}

/************************************************************************/
/* add the scan frequency by 1Hz each time                              */
/************************************************************************/
result_t YDlidarDriver::setScanFrequencyAdd(scan_frequency &frequency,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_AIMSPEED_ADD)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(frequency)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&frequency), sizeof(frequency));
  }
  return RESULT_OK;
}

/************************************************************************/
/* decrease the scan frequency by 1Hz each time                         */
/************************************************************************/
result_t YDlidarDriver::setScanFrequencyDis(scan_frequency &frequency,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_AIMSPEED_DIS)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(frequency)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&frequency), sizeof(frequency));
  }
  return RESULT_OK;
}

/************************************************************************/
/* add the scan frequency by 0.1Hz each time                            */
/************************************************************************/
result_t YDlidarDriver::setScanFrequencyAddMic(scan_frequency &frequency,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_AIMSPEED_ADDMIC)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(frequency)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&frequency), sizeof(frequency));
  }
  return RESULT_OK;
}

/************************************************************************/
/* decrease the scan frequency by 0.1Hz each time                       */
/************************************************************************/
result_t YDlidarDriver::setScanFrequencyDisMic(scan_frequency &frequency,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_AIMSPEED_DISMIC)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(frequency)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&frequency), sizeof(frequency));
  }
  return RESULT_OK;
}

/************************************************************************/
/*  get the sampling rate of lidar                                      */
/************************************************************************/
result_t YDlidarDriver::getSamplingRate(sampling_rate &rate, uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_GET_SAMPLING_RATE)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(rate)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&rate), sizeof(rate));
    m_sampling_rate = rate.rate;
  }
  return RESULT_OK;
}

/************************************************************************/
/*  the set to sampling rate                                            */
/************************************************************************/
result_t YDlidarDriver::setSamplingRate(sampling_rate &rate, uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_SAMPLING_RATE)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != sizeof(rate)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&rate), sizeof(rate));
  }
  return RESULT_OK;
}

/************************************************************************/
/*  the get to zero offset angle                                        */
/************************************************************************/
result_t YDlidarDriver::getZeroOffsetAngle(offset_angle &angle,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_GET_OFFSET_ANGLE)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size < sizeof(offset_angle)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&angle), sizeof(angle));
  }
  return RESULT_OK;
}

/************************************************************************/
/*  the set to heart beat                                               */
/************************************************************************/
result_t YDlidarDriver::setScanHeartbeat(scan_heart_beat &beat,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  disableDataGrabbing();
  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_SET_HEART_BEAT)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size != 1) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&beat), sizeof(beat));
  }
  return RESULT_OK;
}

/************************************************************************/
/*  the get to zero offset angle                                        */
/************************************************************************/
result_t YDlidarDriver::getAutoZeroOffsetAngle(offset_angle &angle,
    uint32_t timeout) {
  result_t  ans;

  if (!m_isConnected) {
    return RESULT_FAIL;
  }

  flushSerial();
  {
    ScopedLocker l(_cmd_lock);

    if ((ans = sendCommand(LIDAR_CMD_GET_OFFSET_ANGLE)) != RESULT_OK) {
      return ans;
    }

    lidar_ans_header response_header;

    if ((ans = waitResponseHeader(&response_header, timeout)) != RESULT_OK) {
      return ans;
    }

    if (response_header.type != LIDAR_ANS_TYPE_DEVINFO) {
      return RESULT_FAIL;
    }

    if (response_header.size < sizeof(offset_angle)) {
      return RESULT_FAIL;
    }

    if (waitForData(response_header.size, timeout) != RESULT_OK) {
      return RESULT_FAIL;
    }

    getData(reinterpret_cast<uint8_t *>(&angle), sizeof(angle));
  }
  return RESULT_OK;
}

std::string YDlidarDriver::getSDKVersion() {
  return YDLIDAR_SDK_VERSION_STR;
}

std::map<std::string, std::string>  YDlidarDriver::lidarPortList() {
  std::vector<PortInfo> lst = list_ports();
  std::map<std::string, std::string> ports;

  for (std::vector<PortInfo>::iterator it = lst.begin(); it != lst.end(); it++) {
    std::string port = "ydlidar" + (*it).device_id;
    ports[port] = (*it).port;
  }

  return ports;
}

result_t YDlidarDriver::parseHeader(
  uint8_t &zero, 
  uint32_t &headPos, 
  uint32_t timeout)
{
  int recvPos = 0;
  uint32_t startTime = getms();
  uint32_t waitTime = 0;
  uint8_t package_type = 0;
  result_t ans = RESULT_TIMEOUT;
  static uint8_t s_buff[PackagePaidBytes * 2] = {0};

  while ((waitTime = getms() - startTime) <= timeout)
  {
    size_t remainSize = PackagePaidBytes - recvPos;
    size_t recvSize = 0;
    ans = waitForData(remainSize, timeout - waitTime, &recvSize);
    if (!IS_OK(ans))
      return ans;

    if (recvSize > remainSize)
      recvSize = remainSize;

    getData(s_buff, recvSize);
    // ans = getData(s_buff, recvSize);
    // if (!IS_OK(ans));
    //   return ans;

    // printf("recv: ");
    // printHex(s_buff, recvSize);

    for (size_t pos = 0; pos < recvSize; ++ pos)
    {
      uint8_t c = s_buff[pos];
      m_dataPos ++;

      // printf("i %u c 0x%02X p %d\n", m_dataPos, c, recvPos);

      switch (recvPos)
      {
      case 0:
        if (c != PH1)
          continue;
        headPos = m_dataPos;
        break;
      case 1:
        if (c != PH2)
        {
          recvPos = 0;
          continue;
        }
        else if (c == PH1)
        {
          recvPos = 1;
          continue;
        }
        break;

      case 2:
        package_type = c & 0x01; //是否是零位包标识
        zero = (package_type == CT_RingStart);
        break;

      case 3:
        // package_Sample_Num = c;
        break;

      case 4:
        if (c & LIDAR_RESP_CHECKBIT)
        {
        }
        else
        {
          recvPos = 0;
          continue;
        }
        break;

      case 5:
        break;

      case 6:
        if (c & LIDAR_RESP_CHECKBIT)
        {
        }
        else
        {
          recvPos = 0;
          continue;
        }
        break;

      case 7:
      case 8:
      case 9:
      default:
        break;
      }

      recvPos ++;
    }

    if (recvPos == PackagePaidBytes)
    {
      ans = RESULT_OK;
      break;
    }

    ans = RESULT_TIMEOUT;
  }

  return ans;
}

#define ZERO_OFFSET12 12 //零位包数据长度12（不带光强）
#define ZERO_OFFSET13 13 //零位包数据长度13（带光强）
result_t YDlidarDriver::getIntensityFlag()
{
  //只针对三角雷达
  if (!isTriangleLidar(m_LidarType))
    return RESULT_OK;

  printf("[YDLIDAR] Start to getting intensity flag\n");
  fflush(stdout);

  m_dataPos = 0;
  uint32_t lastOffset = 0;
  //遍历5圈，如果5圈结果一致则认为准确
  int i = 5;
  while (i-- > 0)
  {
    uint8_t zero = 0; //零位包标记
    uint32_t headPos = 0; //包头位置
    uint32_t offset = 0; //当前圈零位包长度
    uint8_t lastZero = 0; //上一包是否是零位包标记
    uint32_t lastPos = 0; //上一包包头位置
    while (IS_OK(parseHeader(zero, headPos, 500)))
    {
      // printf("zero %u pos %u\n", zero, headPos);
      // fflush(stdout);
      if (zero)
      {
        lastZero = 1;
      }
      else
      {
        if (lastZero)
        {
          lastZero = 0;

          offset = headPos - lastPos;
          // printf("lastPos %u currPos %u offset %u\n", lastPos, headPos, offset);
          //fflush(stdout);

          if (offset != ZERO_OFFSET12 && 
            offset != ZERO_OFFSET13)
            break;
            
          if (lastOffset && 
            lastOffset != offset)
          {
            printf("[YDLIDAR] Fail to getting intensity\n");
            return RESULT_FAIL;
          }

          lastOffset = offset;
          break;
        }
      }

      lastPos = headPos;
    }
  }

  if (lastOffset)
  {
    if (lastOffset == ZERO_OFFSET12)
    {
      setIntensities(false);
    }
    else if (lastOffset == ZERO_OFFSET13)
    {
      setIntensities(true);
      m_intensityBit = 8;
    }
    printf("[YDLIDAR] Auto set intensity %d\n", m_intensities);
  }

  printf("[YDLIDAR] End to getting intensity flag\n");
  fflush(stdout);

  return RESULT_OK;
}


}
