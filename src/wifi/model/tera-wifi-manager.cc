/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2012 The Regents of The University of California 
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

#include "tera-wifi-manager.h"
#include "wifi-phy.h"
#include "ns3/random-variable.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/wifi-mac.h"
#include "ns3/assert.h"
#include <vector>

NS_LOG_COMPONENT_DEFINE ("TeraWifiManager");


namespace ns3 {


struct TeraWifiRemoteStation : public WifiRemoteStation
{
  Time m_nextStatsUpdate;  
  Time m_nextTimeInterval;


  uint32_t m_maxTpRate;  
  uint32_t m_maxTpRate2; 
  uint32_t m_maxProbRate;  
  uint32_t m_shortRetry; 
  uint32_t m_longRetry; 
  uint32_t m_retry;  ///< total retries short + long
  uint32_t m_err;  
  uint32_t m_txrate;  ///< current transmit rate
  uint32_t m_curRate;
  uint32_t m_lastRate;
  uint32_t m_credit;
  uint32_t m_ok;
  uint32_t m_ossilate;
  uint32_t m_consecutive;
  uint32_t m_successive;

  uint32_t m_curTp;
  uint32_t m_lastTp;
  uint32_t m_histTp;


  bool m_isRetry;  ///< a flag to indicate we are currently retrying 
  bool m_initialized;  ///< for initializing tables
  bool m_probing;
  bool m_multiplicative;


  int m_packetCount;  ///< total number of packets as of now
};

NS_OBJECT_ENSURE_REGISTERED (TeraWifiManager);

TypeId
TeraWifiManager::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TeraWifiManager")
    .SetParent<WifiRemoteStationManager> ()
    .AddConstructor<TeraWifiManager> ()
    .AddAttribute ("UpdateStatistics",
                   "The interval between updating statistics table ",
                   TimeValue (Seconds (0.1)),
                   MakeTimeAccessor (&TeraWifiManager::m_updateStats),
                   MakeTimeChecker ())
    .AddAttribute ("UpdateTimeInterval",
                   "The interval between updating statistics table ",
                   TimeValue (Seconds (0.1)),
                   MakeTimeAccessor (&TeraWifiManager::m_updateTimeInterval),
                   MakeTimeChecker ())
    .AddAttribute ("EWMA", 
                   "EWMA level",
                   DoubleValue (75),
                   MakeDoubleAccessor (&TeraWifiManager::m_ewmaLevel),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("EWMA_HIST", 
                   "EWMA level history",
                   DoubleValue (75),
                   MakeDoubleAccessor (&TeraWifiManager::m_ewmaLevelHist),
                   MakeDoubleChecker<double> ())
    .AddAttribute ("RaiseThreshold", "Attempt to raise the rate if we hit that threshold",
                   UintegerValue (10),
                   MakeUintegerAccessor (&TeraWifiManager::m_raiseThreshold),
                   MakeUintegerChecker<uint32_t> ())

    .AddAttribute ("PacketLength",
                   "The packet length used for calculating mode TxTime",
                   DoubleValue (1200),
                   MakeDoubleAccessor (&TeraWifiManager::m_pktLen),
                   MakeDoubleChecker <double> ())
    ;
  return tid;
}

TeraWifiManager::TeraWifiManager ()
{
  m_nsupported = 0;
}

TeraWifiManager::~TeraWifiManager ()
{}

void
TeraWifiManager::SetupPhy (Ptr<WifiPhy> phy)
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
TeraWifiManager::GetCalcTxTime (WifiMode mode) const
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
TeraWifiManager::AddCalcTxTime (WifiMode mode, Time t)
{
   m_calcTxTime.push_back (std::make_pair (t, mode));
}

WifiRemoteStation *
TeraWifiManager::DoCreateStation (void) const
{
  TeraWifiRemoteStation *station = new TeraWifiRemoteStation ();

  station->m_nextStatsUpdate = Simulator::Now () + m_updateStats;
  station->m_nextTimeInterval = Simulator::Now () + m_updateTimeInterval;
  station->m_maxTpRate = 0; 
  station->m_maxTpRate2 = 0; 
  station->m_maxProbRate = 0;
  station->m_packetCount = 0; 
  station->m_isRetry = false; 
  station->m_shortRetry = 0;
  station->m_longRetry = 0;
  station->m_retry = 0;
  station->m_err = 0;
  station->m_txrate = 0;
  station->m_curRate = 0;
  station->m_lastRate = 0;
  station->m_initialized = false;
  station->m_probing = false;
  station->m_multiplicative= false;
  station->m_credit = 0;
  station->m_consecutive = 0;
  station->m_successive = 0;
  station->m_curTp = 0;
  station->m_lastTp = 0;
  station->m_histTp = 0;

  return station;
}

void 
TeraWifiManager::CheckInit(TeraWifiRemoteStation *station)
{
  if (!station->m_initialized && GetNSupported (station) > 1)
    {
      // Note: we appear to be doing late initialization of the table 
      // to make sure that the set of supported rates has been initialized
      // before we perform our own initialization.
      m_nsupported = GetNSupported (station);
      m_teraTable = TeraRate(m_nsupported);
      RateInit (station);
      if ( m_nsupported > 1) 
        {
          ;//station->m_txrate= station->m_curRate = m_nsupported - 1;
        }
      station->m_initialized = true;
    }
}

void
TeraWifiManager::DoReportRxOk (WifiRemoteStation *st,
                                   double rxSnr, WifiMode txMode)
{
  NS_LOG_DEBUG("DoReportRxOk m_txrate=" << ((TeraWifiRemoteStation *)st)->m_txrate);
}

void
TeraWifiManager::DoReportRtsFailed (WifiRemoteStation *st)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *)st;
  NS_LOG_DEBUG("DoReportRtsFailed m_txrate=" << station->m_txrate);

  station->m_shortRetry++;
}

void
TeraWifiManager::DoReportRtsOk (WifiRemoteStation *st, double ctsSnr, WifiMode ctsMode, double rtsSnr)
{
  NS_LOG_DEBUG ("self="<<st<<" rts ok");
}

void
TeraWifiManager::DoReportFinalRtsFailed (WifiRemoteStation *st)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *)st;
  UpdateRetry (station);
  station->m_err++;
}

void
TeraWifiManager::DoReportDataFailed (WifiRemoteStation *st)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *)st;

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
}

void
TeraWifiManager::DoReportDataOk (WifiRemoteStation *st,
                                     double ackSnr, WifiMode ackMode, double dataSnr)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *) st;

  if (station->m_curRate > 0 && station->m_isRetry == true)
    {
      station->m_txrate = station->m_curRate;
      station->m_isRetry = false;
    }
//  station->m_txrate = station->m_curRate;

  CheckInit (station);
  if (!station->m_initialized)
    {
      return;
    }

  m_teraTable[station->m_txrate].numRateSuccess++;
  m_teraTable[station->m_txrate].numRateAttempt++;
	
  UpdateRetry (station);

  m_teraTable[station->m_txrate].numRateAttempt += station->m_retry;
  station->m_ok++; 
  station->m_packetCount++;

}

void
TeraWifiManager::DoReportFinalDataFailed (WifiRemoteStation *st)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoReportFinalDataFailed m_txrate=" << station->m_txrate);

  if (station->m_curRate > 0 && station->m_isRetry == true)
    {
      station->m_txrate = station->m_curRate;
      station->m_isRetry = false;
    }
//  station->m_txrate = station->m_curRate;

  UpdateRetry (station);

  m_teraTable[station->m_txrate].numRateAttempt += station->m_retry;

  
  station->m_err += 1;
}

void
TeraWifiManager::UpdateRetry (TeraWifiRemoteStation *station)
{
  station->m_retry = station->m_shortRetry + station->m_longRetry;
  station->m_shortRetry = 0;
  station->m_longRetry = 0;
}

WifiMode
TeraWifiManager::DoGetDataMode (WifiRemoteStation *st,
                                    uint32_t size)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *) st;
  if (!station->m_initialized)
    {
      CheckInit (station);

    }
  UpdateStats (station) ;
  UpdateAdaptiveTimeInterval(station);

  return GetSupported (station, station->m_txrate);
}

WifiMode
TeraWifiManager::DoGetRtsMode (WifiRemoteStation *st)
{
  TeraWifiRemoteStation *station = (TeraWifiRemoteStation *) st;
  NS_LOG_DEBUG ("DoGetRtsMode m_txrate=" << station->m_txrate);

  return GetSupported (station, 0);
}

bool 
TeraWifiManager::IsLowLatency (void) const
{
  return true;
}
void
TeraWifiManager::UpdateAdaptiveTimeInterval (TeraWifiRemoteStation *station)
{
  if (Simulator::Now () <  station->m_nextTimeInterval)
    {
      return;
    }
//  m_updateTimeInterval = Seconds (0.05);
  station->m_nextTimeInterval = Simulator::Now () + m_updateTimeInterval;

  station->m_ossilate = 0;
}



void
TeraWifiManager::UpdateStats (TeraWifiRemoteStation *station)
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
      txTime = m_teraTable[i].perfectTxTime;       

      /// just for initialization
      if (txTime.GetMicroSeconds () == 0)
        {
          txTime = Seconds (1);
        }

      NS_LOG_DEBUG ("m_txrate=" << station->m_txrate << 
                    "\t attempt=" << m_teraTable[i].numRateAttempt << 
                    "\t success=" << m_teraTable[i].numRateSuccess);

      /// if we've attempted something
      if (m_teraTable[i].numRateAttempt)
        {
          /**
           * calculate the probability of success
           * assume probability scales from 0 to 18000
           */
          tempProb = (m_teraTable[i].numRateSuccess * 18000) / m_teraTable[i].numRateAttempt;

          /// bookeeping
          m_teraTable[i].successHist += m_teraTable[i].numRateSuccess;
          m_teraTable[i].attemptHist += m_teraTable[i].numRateAttempt;
          m_teraTable[i].prob = tempProb;

          /// ewma probability (cast for gcc 3.4 compatibility)
          tempProb = static_cast<uint32_t>(((tempProb * (100 - m_ewmaLevel)) + (m_teraTable[i].ewmaProb * m_ewmaLevel) )/100);

          m_teraTable[i].ewmaProb = tempProb;

          /// calculating throughput
          m_teraTable[i].throughput = tempProb * (1000000 / txTime.GetMicroSeconds());

        }

      /// bookeeping
      m_teraTable[i].prevNumRateAttempt = m_teraTable[i].numRateAttempt;
      m_teraTable[i].prevNumRateSuccess = m_teraTable[i].numRateSuccess;
      m_teraTable[i].numRateSuccess = 0;
      m_teraTable[i].numRateAttempt = 0;

      /// Sample less often below 10% and  above 95% of success
      if ((m_teraTable[i].ewmaProb > 17100) || (m_teraTable[i].ewmaProb < 1800)) 
        {
          /**
           * retry count denotes the number of retries permitted for each rate
           * # retry_count/2
           */
          m_teraTable[i].adjustedRetryCount = m_teraTable[i].retryCount >> 1;
          if (m_teraTable[i].adjustedRetryCount > 2)
            {
              m_teraTable[i].adjustedRetryCount = 2 ;
            }
        }
      else
        {
          m_teraTable[i].adjustedRetryCount = m_teraTable[i].retryCount;
        }

      /// if it's 0 allow one retry limit
      if (m_teraTable[i].adjustedRetryCount == 0)
        {
          m_teraTable[i].adjustedRetryCount = 1;
        }
    }



  uint32_t max_prob = 0, index_max_prob =0, max_tp =0, index_max_tp=0, index_max_tp2=0;

  /// go find max throughput, second maximum throughput, high probability succ
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      NS_LOG_DEBUG ("throughput" << m_teraTable[i].throughput << 
                    "\n ewma" << m_teraTable[i].ewmaProb);

      if (max_tp < m_teraTable[i].throughput) 
        {
          index_max_tp = i;
          max_tp = m_teraTable[i].throughput;
        }

      if (max_prob < m_teraTable[i].ewmaProb) 
        {
          index_max_prob = i;
          max_prob = m_teraTable[i].ewmaProb;
        }
    }


  max_tp = 0;
  /// find the second highest max
  for (uint32_t i =0; i < m_nsupported; i++) 
    {
      if ((i != index_max_tp) && (max_tp < m_teraTable[i].throughput))
        {
          index_max_tp2 = i;
          max_tp = m_teraTable[i].throughput;
        }
    }

  max_tp = 0;

  station->m_maxTpRate = index_max_tp;
  station->m_maxTpRate2 = index_max_tp2;
  station->m_maxProbRate = index_max_prob;

/*
  if (index_max_tp > station->m_txrate)
    {
      station->m_txrate = index_max_tp;
    }
*/

  NS_LOG_DEBUG ("max tp="<< index_max_tp << "\nmax tp2="<< index_max_tp2<< "\nmax prob="<< index_max_prob);



  CheckRate (station);

  /// reset it
  RateInit (station);
}

void
TeraWifiManager::CheckRate(TeraWifiRemoteStation *station)
{
  uint32_t nrate;
  uint32_t orate;
  double delta = 1.0;

  nrate = orate = station->m_curRate;


//  std::cout << "ok " << station->m_ok << " err " << station->m_err << " total " << (station->m_ok + station->m_err) << std::endl;

  if (station->m_consecutive > 1)
    {
      station->m_multiplicative = true;
    }
  else
    {
      station->m_multiplicative = false;
    }


  if (station->m_probing) 
    {
      station->m_probing = false;
      if (m_teraTable[orate].throughput < station->m_histTp && station->m_lastRate != orate )
        {

          station->m_consecutive = 0;
          nrate = station->m_lastRate;

          if (nrate != station->m_txrate)
            {
              NS_ASSERT (nrate < m_nsupported);
              station->m_txrate = nrate;
            }
          station->m_curRate = station->m_txrate;

          station->m_ossilate++;
          if (!station->m_multiplicative)
            {
              m_updateTimeInterval = Seconds(.90);
            }
          else if (station->m_multiplicative)
            {
              m_updateTimeInterval = Seconds(.1);
            }
          station->m_nextTimeInterval = Simulator::Now () + m_updateTimeInterval;

          std::cout << "type " <<  " last tp is better nrate " << station->m_lastRate  <<  "  currate " << station->m_txrate <<"\t curTp " << m_teraTable[orate].throughput << "\t lastTp " << station->m_lastTp << " ossilate " << station->m_ossilate << std::endl;

        }
      else if (m_teraTable[orate].throughput > station->m_lastTp && station->m_lastRate != orate) 
        {
          station->m_consecutive++;
          std::cout << "reseting ossilation: got it right  last tp " << station->m_lastTp <<  " curtp " << m_teraTable[orate].throughput << " last tp is better nrate " << station->m_lastRate  <<  "  currate " << station->m_txrate << std::endl;

          m_updateTimeInterval = Seconds(.1);
          station->m_nextTimeInterval = Simulator::Now () + m_updateTimeInterval;
        }
      return;

    }


  if (station->m_ok + station->m_err > 0) 
    {
      station->m_histTp = static_cast<uint32_t>(((station->m_histTp* (100 - m_ewmaLevelHist)) + ( m_teraTable[orate].throughput * m_ewmaLevelHist) )/100);
    }

  if ( station->m_histTp != 0) 
    {
      delta = (double) m_teraTable[orate].throughput / station->m_histTp;
    }
  else 
    {
      return;
    }
 


  if (delta >= 1 ) 
    {
      station->m_successive = 0;
      if (!station->m_multiplicative && nrate + 1 < m_nsupported && !station->m_ossilate )
        {
          nrate++;
          std::cout << "increase rate to " << nrate << std::endl;
          station->m_probing = true;
        }
      else if (station->m_multiplicative && nrate + 1 < m_nsupported && !station->m_ossilate )
        {
          if (nrate + nrate < m_nsupported)
            {
              nrate = nrate + nrate;
            } 
          else
            {
              nrate = m_nsupported - 1; 
            }
          std::cout << "multiplicative increase rate to " << nrate << std::endl;
          station->m_probing = true;
        }
    }
  else if (delta <= 0.90 && delta >= 0.75) 
    {
      if (nrate > 0) 
        {
          nrate--;
          std::cout << "decrease rate to " << nrate << std::endl;
        }
      station->m_consecutive = 0;
      station->m_ossilate = 0;
    } 
  else if ( delta < 0.75) 
    {
      station->m_successive++;
      if (nrate >= 1 && station->m_successive == 1)
        {
          nrate = nrate * 3/4;
          std::cout << "half decrease rate to " << nrate << std::endl;
        }
      else if (nrate > 0 && station->m_successive > 1)
        {
          nrate--;
          std::cout << "decrease rate to " << nrate << std::endl;
        }
      station->m_consecutive = 0;
      station->m_ossilate = 0;
    }


  std::cout << "currate " << orate  << "\tto  nrate " << nrate <<  "\t curTp " << m_teraTable[orate].throughput << "\t lastTp " << station->m_lastTp  << "\t histTp " << station->m_histTp << "\t  detla " << delta << " ossilate " << station->m_ossilate <<  std::endl;


//  if (m_teraTable[orate].throughput != station->m_lastTp) 
  station->m_lastTp = m_teraTable[station->m_txrate].throughput; 
  station->m_lastRate = station->m_curRate; 

  if (nrate != station->m_txrate)
    {
      NS_ASSERT (nrate < m_nsupported);
      station->m_txrate = nrate;
      station->m_ok = station->m_err = station->m_retry = station->m_credit = 0;
    }

  station->m_curRate = station->m_txrate;
}



void
TeraWifiManager::RateInit (TeraWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("RateInit="<<station);

  for (uint32_t i = 0; i < m_nsupported; i++)
    {
      m_teraTable[i].numRateAttempt = 0;
      m_teraTable[i].numRateSuccess = 0;
      m_teraTable[i].prob = 0;
      m_teraTable[i].ewmaProb = 0;
      m_teraTable[i].prevNumRateAttempt = 0;
      m_teraTable[i].prevNumRateSuccess = 0;
      m_teraTable[i].successHist = 0;
      m_teraTable[i].attemptHist = 0;
      m_teraTable[i].throughput = 0;
      m_teraTable[i].perfectTxTime = GetCalcTxTime (GetSupported (station, i));
      m_teraTable[i].retryCount = 1;
      m_teraTable[i].adjustedRetryCount = 1;
    }
}

void
TeraWifiManager::PrintTable (TeraWifiRemoteStation *station)
{
  NS_LOG_DEBUG ("PrintTable="<<station);

  for (uint32_t i=0; i < m_nsupported; i++)
    {
      std::cout << "index(" << i << ") = " << m_teraTable[i].perfectTxTime<< "\n";
    }
}

} //namespace ns3





