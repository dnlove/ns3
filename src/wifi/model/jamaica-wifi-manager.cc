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
 * 
 */

#include "jamaica-wifi-manager.h"
#include "wifi-phy.h"
#include "ns3/random-variable.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/wifi-mac.h"
#include "ns3/assert.h"
#include <vector>

NS_LOG_COMPONENT_DEFINE ("JamaicaWifiManager");


namespace ns3 {


struct JamaicaWifiRemoteStation : public WifiRemoteStation
{
  Time m_nextStatsUpdate;  

  uint32_t m_maxTpRate;  
  uint32_t m_maxTpRate2; 
  uint32_t m_maxTpRate3; 
  uint32_t m_maxTpRate4; 
  uint32_t m_maxTpRate5; 
  uint32_t m_maxProbRate;  
  uint32_t m_currentRate; 
  uint32_t m_shortRetry; 
  uint32_t m_longRetry; 
  uint32_t m_retry;  ///< total retries short + long
  uint32_t m_err;  
  uint32_t m_txrate;  ///< current transmit rate
  uint32_t m_credit;
  uint32_t m_ok;

  bool m_isRetry;  ///< a flag to indicate we are currently retrying 
  bool m_initialized;  ///< for initializing tables

  int m_packetCount;  ///< total number of packets as of now
};

NS_OBJECT_ENSURE_REGISTERED (JamaicaWifiManager);

TypeId
JamaicaWifiManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::JamaicaWifiManager")
    .SetParent<WifiRemoteStationManager> ()
    .AddConstructor<JamaicaWifiManager> ()
    .AddAttribute ("UpdateStatistics",
                   "The interval between updating statistics table ",
                   TimeValue (Seconds (0.01)),
                   MakeTimeAccessor (&JamaicaWifiManager::m_updateStats),
                   MakeTimeChecker ())
    .AddAttribute ("EWMA", 
                   "EWMA level",
                   DoubleValue (75),
                   MakeDoubleAccessor (&JamaicaWifiManager::m_ewmaLevel),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("PacketLength",
                   "The packet length used for calculating mode TxTime",
                   DoubleValue (1200),
                   MakeDoubleAccessor (&JamaicaWifiManager::m_pktLen),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("RaiseThreshold", "raise the rate if we hit that threshold",
                   UintegerValue (10),
                   MakeUintegerAccessor (&JamaicaWifiManager::m_raiseThreshold),
                   MakeUintegerChecker<double> ())
    .AddAttribute ("AddCreditThreshold", "Add credit threshold",
                   UintegerValue (10),
                   MakeUintegerAccessor (&JamaicaWifiManager::m_addCreditThreshold),
                   MakeUintegerChecker<uint32_t> ())
    ;

    ;
  return tid;
}

JamaicaWifiManager::JamaicaWifiManager ()
{
  m_nsupported = 0;
}

JamaicaWifiManager::~JamaicaWifiManager ()
{}

void
JamaicaWifiManager::SetupPhy (Ptr<WifiPhy> phy)
{
  uint32_t nModes = phy->GetNModes ();
  for (uint32_t i = 0; i < nModes; i++)
    {
      WifiMode mode = phy->GetMode (i);
      AddCalcTxTime (mode, phy->CalculateTxDuration (m_pktLen, mode, WIFI_PREAMBLE_LONG));
    }
  WifiRemoteStationManager::SetupPhy (phy);
}

Time
JamaicaWifiManager::GetCalcTxTime (WifiMode mode) const
{

  for (TxTime::const_iterator i = m_calcTxTime.begin (); i != m_calcTxTime.end (); i++)
    {
      if (mode == i->second)
        {
          return i->first;
        }
    }
  NS_ASSERT (false);
  return Seconds (0);
}

void
JamaicaWifiManager::AddCalcTxTime (WifiMode mode, Time t)
{
   m_calcTxTime.push_back (std::make_pair (t, mode));
}

WifiRemoteStation *
JamaicaWifiManager::DoCreateStation (void) const
{
  JamaicaWifiRemoteStation *station = new JamaicaWifiRemoteStation ();

  station->m_nextStatsUpdate = Simulator::Now () + m_updateStats;
  station->m_maxTpRate = 0; 
  station->m_maxTpRate2 = 0; 
  station->m_maxTpRate3 = 0; 
  station->m_maxTpRate4 = 0; 
  station->m_maxTpRate5 = 0; 
  station->m_maxProbRate = 0;
  station->m_packetCount = 0; 
  station->m_isRetry = false; 
  station->m_currentRate = 0;
  station->m_shortRetry = 0;
  station->m_longRetry = 0;
  station->m_retry = 0;
  station->m_err = 0;
  station->m_txrate = 0;
  station->m_initialized = false;
  station->m_credit = 0;
  station->m_ok = 0;

  return station;
}

void 
JamaicaWifiManager::CheckInit(JamaicaWifiRemoteStation *station)
{
  if (!station->m_initialized && GetNSupported (station) > 1)
    {
      // Note: we appear to be doing late initialization of the table 
      // to make sure that the set of supported rates has been initialized
      // before we perform our own initialization.
      m_nsupported = GetNSupported (station);
      m_jamaicaTable = JamaicaRate(m_nsupported);
      RateInit (station);
      station->m_initialized = true;
    }
}

void
JamaicaWifiManager::DoReportRxOk (WifiRemoteStation *st,
                                   double rxSnr, WifiMode txMode)
{
  NS_LOG_DEBUG("DoReportRxOk m_txrate=" << ((JamaicaWifiRemoteStation *)st)->m_txrate);
}

void
JamaicaWifiManager::DoReportRtsFailed (WifiRemoteStation *st)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *)st;
  NS_LOG_DEBUG("DoReportRtsFailed m_txrate=" << station->m_txrate);

  station->m_shortRetry++;
}

void
JamaicaWifiManager::DoReportRtsOk (WifiRemoteStation *st, double ctsSnr, WifiMode ctsMode, double rtsSnr)
{
  NS_LOG_DEBUG ("self="<<st<<" rts ok");
}

void
JamaicaWifiManager::DoReportFinalRtsFailed (WifiRemoteStation *st)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *)st;
  UpdateRetry (station);
  station->m_err++;
}

void
JamaicaWifiManager::DoReportDataFailed (WifiRemoteStation *st)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *)st;


  CheckInit(station);
  if (!station->m_initialized)
    {
      return;
    }

  station->m_longRetry++;

  if (m_addCreditThreshold + 1 <= 30)
  	m_addCreditThreshold += 1; 

  
  /*
  m_raiseThreshold = static_cast<uint32_t>(((tempProb * (100 - m_ewmaLevel)) + (m_jamaicaTable[i].ewmaProb * m_ewmaLevel) )/100);
    */



  if (station->m_longRetry <= 0)
    {
      station->m_isRetry = true;
    }

  NS_LOG_DEBUG ("DoReportDataFailed " << station << "\t rate " << station->m_txrate << "\tlongRetry \t" << station->m_longRetry);

  if (station->m_longRetry > 6)
    {
      station->m_txrate = 0;
    }
  else if ( station->m_longRetry >= 5)
    {
      if ( station->m_txrate > 0)
        {
          station->m_txrate =  station->m_txrate - 1;
        }
    }
  else 
    {
      CheckRate (station);
    }
}

void
JamaicaWifiManager::DoReportDataOk (WifiRemoteStation *st,
                                     double ackSnr, WifiMode ackMode, double dataSnr)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *) st;

  if (m_addCreditThreshold - 1 >= 10)
  	m_addCreditThreshold -= 1;

  if (station->m_currentRate > 0 && station->m_isRetry == true)
    {
      station->m_txrate = station->m_currentRate;
      station->m_isRetry = false;
    }
  station->m_currentRate = 0;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  m_jamaicaTable[station->m_txrate].numRateSuccess++;
  m_jamaicaTable[station->m_txrate].numRateAttempt++;
	
  UpdateRetry (station);

  m_jamaicaTable[station->m_txrate].numRateAttempt += station->m_retry;
  station->m_ok++; 
  station->m_packetCount++;


  CheckRate (station);
}

void
JamaicaWifiManager::DoReportFinalDataFailed (WifiRemoteStation *st)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoReportFinalDataFailed m_txrate=" << station->m_txrate);


  if (station->m_currentRate > 0 && station->m_isRetry == true)
    {
      station->m_txrate = station->m_currentRate;
      station->m_isRetry = false;
    }
  station->m_currentRate = 0;

  UpdateRetry (station);

  m_jamaicaTable[station->m_txrate].numRateAttempt += station->m_retry;

  
  station->m_err += 1;
}

void
JamaicaWifiManager::UpdateRetry (JamaicaWifiRemoteStation *station)
{
  station->m_retry = station->m_shortRetry + station->m_longRetry;
  station->m_shortRetry = 0;
  station->m_longRetry = 0;
}

WifiMode
JamaicaWifiManager::DoGetDataMode (WifiRemoteStation *st,
                                    uint32_t size)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *) st;
  if (!station->m_initialized)
    {
      CheckInit (station);

      station->m_txrate= m_nsupported - 1;
    }
  UpdateStats (station) ;

  return GetSupported (station, station->m_txrate);
}

WifiMode
JamaicaWifiManager::DoGetRtsMode (WifiRemoteStation *st)
{
  JamaicaWifiRemoteStation *station = (JamaicaWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoGetRtsMode m_txrate=" << station->m_txrate);

  return GetSupported (station, 0);
}

bool 
JamaicaWifiManager::IsLowLatency (void) const
{
  return true;
}

void
JamaicaWifiManager::UpdateStats (JamaicaWifiRemoteStation *station)
{
  if (Simulator::Now () <  station->m_nextStatsUpdate)
    {
      return;
    }

  if (!station->m_initialized)
    {
      return;
    }
  NS_LOG_DEBUG ("Updating stats="<<this);

  station->m_nextStatsUpdate = Simulator::Now () + m_updateStats;

  Time txTime;
  uint32_t tempProb;

  for (uint32_t i =0; i < m_nsupported; i++)
    {        

      /// calculate the perfect tx time for this rate
      txTime = m_jamaicaTable[i].perfectTxTime;       

      /// just for initialization
      if (txTime.GetMicroSeconds () == 0)
        {
          txTime = Seconds (1);
        }

      NS_LOG_DEBUG ("m_txrate=" << station->m_txrate << 
                    "\t attempt=" << m_jamaicaTable[i].numRateAttempt << 
                    "\t success=" << m_jamaicaTable[i].numRateSuccess);

      /// if we've attempted something
      if (m_jamaicaTable[i].numRateAttempt)
        {
          /**
           * calculate the probability of success
           * assume probability scales from 0 to 18000
           */
          tempProb = (m_jamaicaTable[i].numRateSuccess * 18000) / m_jamaicaTable[i].numRateAttempt;

          /// bookeeping
          m_jamaicaTable[i].successHist += m_jamaicaTable[i].numRateSuccess;
          m_jamaicaTable[i].attemptHist += m_jamaicaTable[i].numRateAttempt;
          m_jamaicaTable[i].prob = tempProb;

          /// ewma probability (cast for gcc 3.4 compatibility)
          tempProb = static_cast<uint32_t>(((tempProb * (100 - m_ewmaLevel)) + (m_jamaicaTable[i].ewmaProb * m_ewmaLevel) )/100);

          m_jamaicaTable[i].ewmaProb = tempProb;

          /// calculating throughput
          m_jamaicaTable[i].throughput = tempProb * (1000000 / txTime.GetMicroSeconds());

        }

      /// bookeeping
      m_jamaicaTable[i].prevNumRateAttempt = m_jamaicaTable[i].numRateAttempt;
      m_jamaicaTable[i].prevNumRateSuccess = m_jamaicaTable[i].numRateSuccess;
      m_jamaicaTable[i].numRateSuccess = 0;
      m_jamaicaTable[i].numRateAttempt = 0;

      /// Sample less often below 10% and  above 95% of success
      if ((m_jamaicaTable[i].ewmaProb > 17100) || (m_jamaicaTable[i].ewmaProb < 1800)) 
        {
          /**
           * retry count denotes the number of retries permitted for each rate
           * # retry_count/2
           */
          m_jamaicaTable[i].adjustedRetryCount = m_jamaicaTable[i].retryCount >> 1;
          if (m_jamaicaTable[i].adjustedRetryCount > 2)
            {
              m_jamaicaTable[i].adjustedRetryCount = 2 ;
            }
        }
      else
        {
          m_jamaicaTable[i].adjustedRetryCount = m_jamaicaTable[i].retryCount;
        }

      /// if it's 0 allow one retry limit
      if (m_jamaicaTable[i].adjustedRetryCount == 0)
        {
          m_jamaicaTable[i].adjustedRetryCount = 1;
        }
    }


  uint32_t max_prob = 0, index_max_prob =0, max_tp =0, index_max_tp=0, index_max_tp2=0;
  uint32_t index_max_tp3 = 0, index_max_tp4 = 0, index_max_tp5 = 0;

  /// go find max throughput, second maximum throughput, high probability succ
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      NS_LOG_DEBUG ("throughput" << m_jamaicaTable[i].throughput << 
                    "\n ewma" << m_jamaicaTable[i].ewmaProb);

      if (max_tp < m_jamaicaTable[i].throughput) 
        {
          index_max_tp = i;
          max_tp = m_jamaicaTable[i].throughput;
        }

      if (max_prob < m_jamaicaTable[i].ewmaProb) 
        {
          index_max_prob = i;
          max_prob = m_jamaicaTable[i].ewmaProb;
        }
    }


  max_tp = 0;
  /// find the second highest max
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      if ((i != index_max_tp) && (max_tp < m_jamaicaTable[i].throughput))
        {
          index_max_tp2 = i;
          max_tp = m_jamaicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the third highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2) &&
          ( max_tp < m_jamaicaTable[i].throughput))
        {
          index_max_tp3 = i;
          max_tp = m_jamaicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the fourth highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2 && i != index_max_tp3) &&
          ( max_tp < m_jamaicaTable[i].throughput))
        {
          index_max_tp4 = i;
          max_tp = m_jamaicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the fifth highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2 && i != index_max_tp3 && i != index_max_tp4) &&
          ( max_tp < m_jamaicaTable[i].throughput))
        {
          index_max_tp5 = i;
          max_tp = m_jamaicaTable[i].throughput;
        }
    }

  station->m_maxTpRate = index_max_tp;
  station->m_maxTpRate2 = index_max_tp2;
  station->m_maxTpRate3 = index_max_tp3;
  station->m_maxTpRate4 = index_max_tp4;
  station->m_maxTpRate5 = index_max_tp5;
  station->m_maxProbRate = index_max_prob;

  if (index_max_tp > station->m_txrate)
    {
      station->m_txrate = index_max_tp;
    }

  NS_LOG_DEBUG ("max tp="<< index_max_tp << "\nmax tp2="<< index_max_tp2<< "\nmax prob="<< index_max_prob);

  CheckRate (station);

  /// reset it
  RateInit (station);
}

void
JamaicaWifiManager::CheckRate(JamaicaWifiRemoteStation *station)
{
  int enough;
  double errorThres = station->m_ok * 3 / 10; 
  uint32_t nrate;


  std::cout <<  "m_raise  " << m_addCreditThreshold << std::endl;


  enough = (station->m_ok + station->m_err >= m_addCreditThreshold);

  nrate = station->m_txrate;

  if (enough && station->m_err > 0 && station->m_ok == 0)
    {
      if (nrate > 0) 
        {
          nrate--;
	}
	station->m_credit = 0;
    }

  if (enough && station->m_ok < station->m_retry)
    {
      if (nrate > 0) 
        {
          nrate--;
	}
	station->m_credit = 0;
    }

  if (enough && station->m_err > errorThres)
    {
      if (nrate > 0) 
        {
          nrate--;
	}
        if (enough && station->m_ok < station->m_err)
	  {
	    if (nrate >= 4)
	      {
	        nrate = nrate * 3/4;
              }
	  }
	station->m_credit = 0;
    }      

  if (enough && station->m_err <= errorThres)
    {
      station->m_credit++; 
    }



  if (station->m_credit >= m_raiseThreshold)
    {
      station->m_credit = 0;
      if (nrate + 1 < m_nsupported)
        {
            nrate++;
        }
    }

  if (nrate != station->m_txrate)
    {
      NS_ASSERT (nrate < m_nsupported);
      station->m_txrate = nrate;
      station->m_ok = station->m_err = station->m_retry = station->m_credit = 0;
    }
  else if (enough)
    {
      station->m_ok = station->m_err = station->m_retry = 0;
    }

  station->m_currentRate = station->m_txrate;
}



void
JamaicaWifiManager::RateInit (JamaicaWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("RateInit="<<station);

  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      m_jamaicaTable[i].numRateAttempt = 0;
      m_jamaicaTable[i].numRateSuccess = 0;
      m_jamaicaTable[i].prob = 0;
      m_jamaicaTable[i].ewmaProb = 0;
      m_jamaicaTable[i].prevNumRateAttempt = 0;
      m_jamaicaTable[i].prevNumRateSuccess = 0;
      m_jamaicaTable[i].successHist = 0;
      m_jamaicaTable[i].attemptHist = 0;
      m_jamaicaTable[i].throughput = 0;
      m_jamaicaTable[i].perfectTxTime = GetCalcTxTime (GetSupported (station, i));
      m_jamaicaTable[i].retryCount = 1;
      m_jamaicaTable[i].adjustedRetryCount = 1;
    }
}

void
JamaicaWifiManager::PrintTable (JamaicaWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("PrintTable="<<station);

  for (uint32_t i=0; i < m_nsupported; i++)
    {
      std::cout << "index(" << i << ") = " << m_jamaicaTable[i].perfectTxTime<< "\n";
    }
}

} //namespace ns3





