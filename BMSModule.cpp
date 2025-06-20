#include "config.h"
#include "BMSModule.h"
#include "BMSUtil.h"


BMSModule::BMSModule()
{
  for (int i = 0; i < 16; i++)
  {
    cellVolt[i] = 0.0f;
    lowestCellVolt[i] = 5.0f;
    highestCellVolt[i] = 0.0f;
  }
  moduleVolt = 0.0f;
  temperatures[0] = 0.0f;
  temperatures[1] = 0.0f;
  temperatures[2] = 0.0f;
  temperatures[3] = 0.0f;
  lowestTemperature = 200.0f;
  highestTemperature = -100.0f;
  lowestModuleVolt = 200.0f;
  highestModuleVolt = 0.0f;
  exists = false;
  reset = false;
  moduleAddress = 0;
  error = 0;
}

void BMSModule::clearmodule()
{
  for (int i = 0; i < 16; i++)
  {
    cellVolt[i] = 0.0f;
  }
  moduleVolt = 0.0f;
  temperatures[0] = 0.0f;
  temperatures[1] = 0.0f;
  temperatures[2] = 0.0f;
  temperatures[3] = 0.0f;
  variant = 0;
  exists = false;
  reset = false;
  moduleAddress = 0;
}

void BMSModule::decodetemp(CAN_message_t &msg, int CSC)
{
  for (int g = 0; g < 4; g++)
  {
    temperatures[g] = msg.buf[g] - 40;
    if (temperatures[g] > -40)
    {
      temperatures[g] = temperatures[g] + TempOff;
    }
  }
}

void BMSModule::decodecan(int Id, CAN_message_t &msg, bool Ign)
{
  switch (Id)
  {
    case 0:
      error = msg.buf[0] + (msg.buf[1] << 8) + (msg.buf[2] << 16) + (msg.buf[3] << 24);
      balstat = (msg.buf[5]<< 8) + msg.buf[4];
      break;

    case 1:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[0] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
        cellVolt[1] = float(msg.buf[2] + (msg.buf[3] & 0x3F) * 256) / 1000;
        cellVolt[2] = float(msg.buf[4] + (msg.buf[5] & 0x3F) * 256) / 1000;
      }
      break;

    case 2:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[3] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
        cellVolt[4] = float(msg.buf[2] + (msg.buf[3] & 0x3F) * 256) / 1000;
        cellVolt[5] = float(msg.buf[4] + (msg.buf[5] & 0x3F) * 256) / 1000;
      }
      break;

    case 3:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[6] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
        cellVolt[7] = float(msg.buf[2] + (msg.buf[3] & 0x3F) * 256) / 1000;
        cellVolt[8] = float(msg.buf[4] + (msg.buf[5] & 0x3F) * 256) / 1000;
      }
      break;

    case 4:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[9] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
        cellVolt[10] = float(msg.buf[2] + (msg.buf[3] & 0x3F) * 256) / 1000;
        cellVolt[11] = float(msg.buf[4] + (msg.buf[5] & 0x3F) * 256) / 1000;
      }
      break;

    case 5:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[12] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
        cellVolt[13] = float(msg.buf[2] + (msg.buf[3] & 0x3F) * 256) / 1000;
        cellVolt[14] = float(msg.buf[4] + (msg.buf[5] & 0x3F) * 256) / 1000;
      }
      break;


    case 6:
      if (balstat == 0 && Ign == 0)
      {
        cellVolt[15] = float(msg.buf[0] + (msg.buf[1] & 0x3F) * 256) / 1000;
      }
      break;

    default:

      break;
  }
  for (int i = 0; i < 16; i++)
  {
    if (lowestCellVolt[i] > cellVolt[i] && cellVolt[i] >= IgnoreCell) lowestCellVolt[i] = cellVolt[i];
    if (highestCellVolt[i] < cellVolt[i] && cellVolt[i] > 5.0) highestCellVolt[i] = cellVolt[i];
  }
}


/*
  Reading the status of the board to identify any flags, will be more useful when implementing a sleep cycle
*/

uint8_t BMSModule::getFaults()
{
  return faults;
}

uint8_t BMSModule::getAlerts()
{
  return alerts;
}

uint8_t BMSModule::getCOVCells()
{
  return COVFaults;
}

uint8_t BMSModule::getCUVCells()
{
  return CUVFaults;
}

/*
  Reading the setpoints, after a reset the default tesla setpoints are loaded
  Default response : 0x10, 0x80, 0x31, 0x81, 0x08, 0x81, 0x66, 0xff
*/
/*
  void BMSModule::readSetpoint()
  {
  uint8_t payload[3];
  uint8_t buff[12];
  payload[0] = moduleAddress << 1; //adresss
  payload[1] = 0x40;//Alert Status start
  payload[2] = 0x08;//two registers
  sendData(payload, 3, false);
  delay(2);
  getReply(buff);

  OVolt = 2.0+ (0.05* buff[5]);
  UVolt = 0.7 + (0.1* buff[7]);
  Tset = 35 + (5 * (buff[9] >> 4));
  } */

float BMSModule::getCellVoltage(int cell)
{
  if (cell < 0 || cell > 16) return 0.0f;
  return cellVolt[cell];
}

float BMSModule::getLowCellV()
{
  float lowVal = 10.0f;
  for (int i = 0; i < 16; i++) if (cellVolt[i] < lowVal && cellVolt[i] > IgnoreCell) lowVal = cellVolt[i];
  return lowVal;
}

float BMSModule::getHighCellV()
{
  float hiVal = 0.0f;
  for (int i = 0; i < 16; i++)
    if (cellVolt[i] > IgnoreCell && cellVolt[i] < 5.0)
    {
      if (cellVolt[i] > hiVal) hiVal = cellVolt[i];
    }
  return hiVal;
}

float BMSModule::getAverageV()
{
  int x = 0;
  float avgVal = 0.0f;
  for (int i = 0; i < 8; i++)
  {
    if (cellVolt[i] > IgnoreCell && cellVolt[i] < 5.0)
    {
      x++;
      avgVal += cellVolt[i];
    }
  }

  scells = x;
  if (x > 0) {
    avgVal /= x;
  }
  return avgVal;
}

int BMSModule::getscells()
{
  return scells;
}

int BMSModule::getbalstat()
{
  return balstat;
}

float BMSModule::getHighestModuleVolt()
{
  return highestModuleVolt;
}

float BMSModule::getLowestModuleVolt()
{
  return lowestModuleVolt;
}

float BMSModule::getHighestCellVolt(int cell)
{
  if (cell < 0 || cell > 16) return 0.0f;
  return highestCellVolt[cell];
}

float BMSModule::getLowestCellVolt(int cell)
{
  if (cell < 0 || cell > 16) return 0.0f;
  return lowestCellVolt[cell];
}

float BMSModule::getHighestTemp()
{
  return highestTemperature;
}

float BMSModule::getLowestTemp()
{
  return lowestTemperature;
}

float BMSModule::getLowTemp()
{
  /*
    float templow = 9999;
    for (int g = 0; g < 4; g++)
    {
    if (temperatures[g] < templow && temperatures[g] > -40)
    {
      templow = temperatures[g];
    }
    }
    return (templow);
  */
  if (temperatures[0] < temperatures[1] )
  {
    return (temperatures[0]);
  }
  else
  {
    return (temperatures[1]);
  }
}

float BMSModule::getHighTemp()
{
  /*
    float temphigh = -39;
    for (int g = 0; g < 4; g++)
    {
    if (temperatures[g] > temphigh && temperatures[g] > -40)
    {
      temphigh = temperatures[g];
    }
    }
    return (temphigh);
  */

  if (temperatures[0] > temperatures[1])
  {
    return (temperatures[0]);
  }
  else
  {
    return (temperatures[1]);
  }
}

float BMSModule::getAvgTemp()
{
  float avgtemp = 0;
  int num = 0;
  for (int g = 0; g < 2; g++)
  {
    if (temperatures[g] > -40)
    {
      avgtemp = avgtemp + temperatures[g];
      num++;
    }
  }
  avgtemp = avgtemp / (float)(num);
  return (avgtemp);
}

float BMSModule::getModuleVoltage()
{
  moduleVolt = 0;
  for (int I; I < 16; I++)
  {
    if (cellVolt[I] > IgnoreCell && cellVolt[I] < 5.0)
    {
      moduleVolt = moduleVolt + cellVolt[I];
    }
  }
  return moduleVolt;
}

float BMSModule::getTemperature(int temp)
{
  if (temp < 0 || temp > 3) return 0.0f;
  return temperatures[temp];
}

void BMSModule::setAddress(int newAddr)
{
  if (newAddr < 0 || newAddr > MAX_MODULE_ADDR) return;
  moduleAddress = newAddr;
}

int BMSModule::getAddress()
{
  return moduleAddress;
}

uint32_t BMSModule::getError()
{
  return error;
}

bool BMSModule::isExisting()
{
  return exists;
}

bool BMSModule::isReset()
{
  return reset;
}


void BMSModule::settempsensor(int tempsensor)
{
  sensor = tempsensor;
}

void BMSModule::setExists(bool ex)
{
  exists = ex;
}

void BMSModule::setReset(bool ex)
{
  reset = ex;
}

void BMSModule::setIgnoreCell(float Ignore)
{
  IgnoreCell = Ignore;
  /*
    Serial.println();
    Serial.println();
    Serial.println(Ignore);
    Serial.println();
  */
}

void  BMSModule::setTempOff( int16_t tempoff)
{
  TempOff = tempoff;
}
