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

#include "maica-wifi-manager.h"
#include "wifi-phy.h"
#include "ns3/random-variable.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/wifi-mac.h"
#include "ns3/assert.h"
#include <vector>

NS_LOG_COMPONENT_DEFINE ("MaicaWifiManager");


namespace ns3 {


struct MaicaWifiRemoteStation : public WifiRemoteStation
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

NS_OBJECT_ENSURE_REGISTERED (MaicaWifiManager);

TypeId
MaicaWifiManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MaicaWifiManager")
    .SetParent<WifiRemoteStationManager> ()
    .AddConstructor<MaicaWifiManager> ()
    .AddAttribute ("UpdateStatistics",
                   "The interval between updating statistics table ",
                   TimeValue (Seconds (0.1)),
                   MakeTimeAccessor (&MaicaWifiManager::m_updateStats),
                   MakeTimeChecker ())
    .AddAttribute ("EWMA", 
                   "EWMA level",
                   DoubleValue (75),
                   MakeDoubleAccessor (&MaicaWifiManager::m_ewmaLevel),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("PacketLength",
                   "The packet length used for calculating mode TxTime",
                   DoubleValue (1200),
                   MakeDoubleAccessor (&MaicaWifiManager::m_pktLen),
                   MakeDoubleChecker <double> ())
    .AddAttribute ("RaiseThreshold", "raise the rate if we hit that threshold",
                   UintegerValue (10),
                   MakeUintegerAccessor (&MaicaWifiManager::m_raiseThreshold),
                   MakeUintegerChecker<double> ())
    .AddAttribute ("AddCreditThreshold", "Add credit threshold",
                   UintegerValue (10),
                   MakeUintegerAccessor (&MaicaWifiManager::m_addCreditThreshold),
                   MakeUintegerChecker<uint32_t> ())
    ;

    ;
  return tid;
}

MaicaWifiManager::MaicaWifiManager ()
{
  m_nsupported = 0;
}

MaicaWifiManager::~MaicaWifiManager ()
{}

void
MaicaWifiManager::SetupPhy (Ptr<WifiPhy> phy)
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
MaicaWifiManager::GetCalcTxTime (WifiMode mode) const
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
MaicaWifiManager::AddCalcTxTime (WifiMode mode, Time t)
{
   m_calcTxTime.push_back (std::make_pair (t, mode));
}

WifiRemoteStation *
MaicaWifiManager::DoCreateStation (void) const
{
  MaicaWifiRemoteStation *station = new MaicaWifiRemoteStation ();

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
MaicaWifiManager::CheckInit(MaicaWifiRemoteStation *station)
{
  if (!station->m_initialized && GetNSupported (station) > 1)
    {
      // Note: we appear to be doing late initialization of the table 
      // to make sure that the set of supported rates has been initialized
      // before we perform our own initialization.
      m_nsupported = GetNSupported (station);
      m_maicaTable = MaicaRate(m_nsupported);
      RateInit (station);
      station->m_initialized = true;
    }
}

void
MaicaWifiManager::DoReportRxOk (WifiRemoteStation *st,
                                   double rxSnr, WifiMode txMode)
{
  NS_LOG_DEBUG("DoReportRxOk m_txrate=" << ((MaicaWifiRemoteStation *)st)->m_txrate);
}

void
MaicaWifiManager::DoReportRtsFailed (WifiRemoteStation *st)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *)st;
  NS_LOG_DEBUG("DoReportRtsFailed m_txrate=" << station->m_txrate);

  station->m_shortRetry++;
}

void
MaicaWifiManager::DoReportRtsOk (WifiRemoteStation *st, double ctsSnr, WifiMode ctsMode, double rtsSnr)
{
  NS_LOG_DEBUG ("self="<<st<<" rts ok");
}

void
MaicaWifiManager::DoReportFinalRtsFailed (WifiRemoteStation *st)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *)st;
  UpdateRetry (station);
  station->m_err++;
}

void
MaicaWifiManager::DoReportDataFailed (WifiRemoteStation *st)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *)st;

  CheckInit(station);
  if (!station->m_initialized)
    {
      return;
    }

  station->m_longRetry++;

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
MaicaWifiManager::DoReportDataOk (WifiRemoteStation *st,
                                     double ackSnr, WifiMode ackMode, double dataSnr)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *) st;

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

  m_maicaTable[station->m_txrate].numRateSuccess++;
  m_maicaTable[station->m_txrate].numRateAttempt++;
	
  UpdateRetry (station);

  m_maicaTable[station->m_txrate].numRateAttempt += station->m_retry;
  station->m_ok++; 
  station->m_packetCount++;

}

void
MaicaWifiManager::DoReportFinalDataFailed (WifiRemoteStation *st)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoReportFinalDataFailed m_txrate=" << station->m_txrate);

  if (station->m_currentRate > 0 && station->m_isRetry == true)
    {
      station->m_txrate = station->m_currentRate;
      station->m_isRetry = false;
    }
  station->m_currentRate = 0;

  UpdateRetry (station);

  m_maicaTable[station->m_txrate].numRateAttempt += station->m_retry;

  
  station->m_err += 1;
}

void
MaicaWifiManager::UpdateRetry (MaicaWifiRemoteStation *station)
{
  station->m_retry = station->m_shortRetry + station->m_longRetry;
  station->m_shortRetry = 0;
  station->m_longRetry = 0;
}

WifiMode
MaicaWifiManager::DoGetDataMode (WifiRemoteStation *st,
                                    uint32_t size)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *) st;
  if (!station->m_initialized)
    {
      CheckInit (station);

      if ( m_nsupported > 1) 
        {
          station->m_txrate= m_nsupported / 2;
        }
    }
  UpdateStats (station) ;

  return GetSupported (station, station->m_txrate);
}

WifiMode
MaicaWifiManager::DoGetRtsMode (WifiRemoteStation *st)
{
  MaicaWifiRemoteStation *station = (MaicaWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoGetRtsMode m_txrate=" << station->m_txrate);

  return GetSupported (station, 0);
}

bool 
MaicaWifiManager::IsLowLatency (void) const
{
  return true;
}

void
MaicaWifiManager::UpdateStats (MaicaWifiRemoteStation *station)
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
      txTime = m_maicaTable[i].perfectTxTime;       

      /// just for initialization
      if (txTime.GetMicroSeconds () == 0)
        {
          txTime = Seconds (1);
        }

      NS_LOG_DEBUG ("m_txrate=" << station->m_txrate << 
                    "\t attempt=" << m_maicaTable[i].numRateAttempt << 
                    "\t success=" << m_maicaTable[i].numRateSuccess);

      /// if we've attempted something
      if (m_maicaTable[i].numRateAttempt)
        {
          /**
           * calculate the probability of success
           * assume probability scales from 0 to 18000
           */
          tempProb = (m_maicaTable[i].numRateSuccess * 18000) / m_maicaTable[i].numRateAttempt;

          /// bookeeping
          m_maicaTable[i].successHist += m_maicaTable[i].numRateSuccess;
          m_maicaTable[i].attemptHist += m_maicaTable[i].numRateAttempt;
          m_maicaTable[i].prob = tempProb;

          /// ewma probability (cast for gcc 3.4 compatibility)
          tempProb = static_cast<uint32_t>(((tempProb * (100 - m_ewmaLevel)) + (m_maicaTable[i].ewmaProb * m_ewmaLevel) )/100);

          m_maicaTable[i].ewmaProb = tempProb;

          /// calculating throughput
          m_maicaTable[i].throughput = tempProb * (1000000 / txTime.GetMicroSeconds());

        }

      /// bookeeping
      m_maicaTable[i].prevNumRateAttempt = m_maicaTable[i].numRateAttempt;
      m_maicaTable[i].prevNumRateSuccess = m_maicaTable[i].numRateSuccess;
      m_maicaTable[i].numRateSuccess = 0;
      m_maicaTable[i].numRateAttempt = 0;

      /// Sample less often below 10% and  above 95% of success
      if ((m_maicaTable[i].ewmaProb > 17100) || (m_maicaTable[i].ewmaProb < 1800)) 
        {
          /**
           * retry count denotes the number of retries permitted for each rate
           * # retry_count/2
           */
          m_maicaTable[i].adjustedRetryCount = m_maicaTable[i].retryCount >> 1;
          if (m_maicaTable[i].adjustedRetryCount > 2)
            {
              m_maicaTable[i].adjustedRetryCount = 2 ;
            }
        }
      else
        {
          m_maicaTable[i].adjustedRetryCount = m_maicaTable[i].retryCount;
        }

      /// if it's 0 allow one retry limit
      if (m_maicaTable[i].adjustedRetryCount == 0)
        {
          m_maicaTable[i].adjustedRetryCount = 1;
        }
    }


  uint32_t max_prob = 0, index_max_prob =0, max_tp =0, index_max_tp=0, index_max_tp2=0;
  uint32_t index_max_tp3 = 0, index_max_tp4 = 0, index_max_tp5 = 0;

  /// go find max throughput, second maximum throughput, high probability succ
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      NS_LOG_DEBUG ("throughput" << m_maicaTable[i].throughput << 
                    "\n ewma" << m_maicaTable[i].ewmaProb);

      if (max_tp < m_maicaTable[i].throughput) 
        {
          index_max_tp = i;
          max_tp = m_maicaTable[i].throughput;
        }

      if (max_prob < m_maicaTable[i].ewmaProb) 
        {
          index_max_prob = i;
          max_prob = m_maicaTable[i].ewmaProb;
        }
    }


  max_tp = 0;
  /// find the second highest max
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      if ((i != index_max_tp) && (max_tp < m_maicaTable[i].throughput))
        {
          index_max_tp2 = i;
          max_tp = m_maicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the third highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2) &&
          ( max_tp < m_maicaTable[i].throughput))
        {
          index_max_tp3 = i;
          max_tp = m_maicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the fourth highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2 && i != index_max_tp3) &&
          ( max_tp < m_maicaTable[i].throughput))
        {
          index_max_tp4 = i;
          max_tp = m_maicaTable[i].throughput;
        }
    }

  max_tp = 0;
  /// find the fifth highest max
  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      if (( i != index_max_tp && i != index_max_tp2 && i != index_max_tp3 && i != index_max_tp4) &&
          ( max_tp < m_maicaTable[i].throughput))
        {
          index_max_tp5 = i;
          max_tp = m_maicaTable[i].throughput;
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
MaicaWifiManager::CheckRate(MaicaWifiRemoteStation *station)
{
  int enough;
  double errorThres = station->m_ok * 3 / 10; 
  uint32_t nrate;

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
MaicaWifiManager::RateInit (MaicaWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("RateInit="<<station);

  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      m_maicaTable[i].numRateAttempt = 0;
      m_maicaTable[i].numRateSuccess = 0;
      m_maicaTable[i].prob = 0;
      m_maicaTable[i].ewmaProb = 0;
      m_maicaTable[i].prevNumRateAttempt = 0;
      m_maicaTable[i].prevNumRateSuccess = 0;
      m_maicaTable[i].successHist = 0;
      m_maicaTable[i].attemptHist = 0;
      m_maicaTable[i].throughput = 0;
      m_maicaTable[i].perfectTxTime = GetCalcTxTime (GetSupported (station, i));
      m_maicaTable[i].retryCount = 1;
      m_maicaTable[i].adjustedRetryCount = 1;
    }
}

void
MaicaWifiManager::PrintTable (MaicaWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("PrintTable="<<station);

  for (uint32_t i=0; i < m_nsupported; i++)
    {
      std::cout << "index(" << i << ") = " << m_maicaTable[i].perfectTxTime<< "\n";
    }
}

} //namespace ns3





