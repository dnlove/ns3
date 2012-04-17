/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/* 
 * Copyright (c) 2011 The Regents of The University of California 
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 * Author: Duy Nguyen <duy@soe.ucsc.edu> 
 */



#ifndef JAMAICA_WIFI_MANAGER_H
#define JAMAICA_WIFI_MANAGER_H

#include "wifi-remote-station-manager.h"
#include "minstrel-wifi-manager.h"
#include "wifi-mode.h"
#include "ns3/nstime.h"
#include <vector>

/**
 * \author Duy Nguyen
 * \brief Implementation of Jamaica Rate Control Algorithm 
 *
 */


namespace ns3 {

class JamaicaWifiRemoteStation;

/**
 * Data structure for a Jamaica Rate table
 * A vector of a struct RateInfo
 */
typedef std::vector<struct RateInfo> JamaicaRate;


class JamaicaWifiManager : public WifiRemoteStationManager
{

public:
  static TypeId GetTypeId (void);
  JamaicaWifiManager ();
  virtual ~JamaicaWifiManager();

  virtual void SetupPhy (Ptr<WifiPhy> phy);

private:
  // overriden from base class
  virtual class WifiRemoteStation *DoCreateStation (void) const;

  virtual void DoReportRxOk (WifiRemoteStation *station, 
                             double rxSnr, WifiMode txMode);
  virtual void DoReportRtsFailed (WifiRemoteStation *station);
  virtual void DoReportDataFailed (WifiRemoteStation *station);
  virtual void DoReportRtsOk (WifiRemoteStation *station,
                              double ctsSnr, WifiMode ctsMode, double rtsSnr);
  virtual void DoReportDataOk (WifiRemoteStation *station,
                               double ackSnr, WifiMode ackMode, double dataSnr);
  virtual void DoReportFinalRtsFailed (WifiRemoteStation *station);
  virtual void DoReportFinalDataFailed (WifiRemoteStation *station);

  virtual WifiMode DoGetDataMode (WifiRemoteStation *station, uint32_t size);
  virtual WifiMode DoGetRtsMode (WifiRemoteStation *station);

  virtual bool IsLowLatency (void) const;

  Time GetCalcTxTime (WifiMode mode) const;

  void AddCalcTxTime (WifiMode mode, Time t);

  void UpdateRetry (JamaicaWifiRemoteStation *station);

  void UpdateStats (JamaicaWifiRemoteStation *station);

  void RateInit (JamaicaWifiRemoteStation *station);

  void PrintTable (JamaicaWifiRemoteStation *station);

  void CheckInit (JamaicaWifiRemoteStation *station); 

  void CheckRate (JamaicaWifiRemoteStation *station);  

  typedef std::vector<std::pair<Time,WifiMode> > TxTime;

  JamaicaRate m_jamaicaTable;  ///< jamaica table
  TxTime m_calcTxTime;  ///< to hold all the calculated TxTime for all modes
  Time m_updateStats;  ///< how frequent do we calculate the stats(1/10 seconds)
  double m_ewmaLevel;  ///< exponential weighted moving average
  uint32_t m_pktLen;  ///< packet length used  for calculate mode TxTime  
  uint32_t m_nsupported;  ///< modes supported
  uint32_t m_addCreditThreshold;
  double m_raiseThreshold;
};

}// namespace ns3

#endif /* MAICA_WIFI_MANAGER_H */
