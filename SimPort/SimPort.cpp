/*	opendatacon
 *
 *	Copyright (c) 2014:
 *
 *		DCrip3fJguWgVCLrZFfA7sIGgvx1Ou3fHfCxnrz4svAi
 *		yxeOtDhDCXf1Z4ApgXvX5ahqQmzRfJ2DoX8S05SqHA==
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */
/*
 * SimPort.h
 *
 *  Created on: 29/07/2015
 *      Author: Neil Stephens <dearknarl@gmail.com>
 */
#include <memory>
#include <random>
#include <limits>
#include <opendatacon/util.h>
#include <opendatacon/IOTypes.h>
#include "SimPort.h"
#include "SimPortConf.h"
#include "SimPortCollection.h"

inline unsigned int random_interval(const unsigned int& average_interval, rand_t& seed)
{
	//random interval - uniform distribution, minimum 1ms
	auto ret_val = (unsigned int)((2*average_interval-2)*ZERO_TO_ONE(seed)+1.5); //the .5 is for rounding down
	return ret_val;
}

//Implement DataPort interface
SimPort::SimPort(const std::string& Name, const std::string& File, const Json::Value& Overrides):
	DataPort(Name, File, Overrides),
	SimCollection(nullptr)
{

	static std::atomic_flag init_flag = ATOMIC_FLAG_INIT;
	static std::weak_ptr<SimPortCollection> weak_collection;

	//if we're the first/only one on the scene,
	// init the SimPortCollection
	if(!init_flag.test_and_set(std::memory_order_acquire))
	{
		//make a custom deleter for the DNP3Manager that will also clear the init flag
		auto deinit_del = [](SimPortCollection* collection_ptr)
					{init_flag.clear(); delete collection_ptr;};
		this->SimCollection = std::shared_ptr<SimPortCollection>(new SimPortCollection(), deinit_del);
		weak_collection = this->SimCollection;
	}
	//otherwise just make sure it's finished initialising and take a shared_ptr
	else
	{
		while (!(this->SimCollection = weak_collection.lock()))
		{} //init happens very seldom, so spin lock is good
	}

	pConf.reset(new SimPortConf());
	ProcessFile();
}
void SimPort::Enable()
{
	pEnableDisableSync->post([&]()
		{
			if(!enabled)
			{
			      enabled = true;
			      PortUp();
			}
		});
}
void SimPort::Disable()
{
	pEnableDisableSync->post([&]()
		{
			if(enabled)
			{
			      enabled = false;
			      PortDown();
			}
		});
}

std::pair<std::string, std::shared_ptr<IUIResponder> > SimPort::GetUIResponder()
{
	return std::pair<std::string,std::shared_ptr<SimPortCollection>>("SimControl",this->SimCollection);
}

bool SimPort::Force(const std::string& type, const std::string& index, const std::string& value, const std::string& quality)
{
	size_t idx; double val;
	try
	{
		idx = std::stoi(index);
		val = std::stod(value);
	}
	catch(std::invalid_argument e)
	{
		return false;
	}

	if(type == "Binary")
	{
		{ //lock scope
			auto pConf = static_cast<SimPortConf*>(this->pConf.get());
			std::unique_lock<std::shared_timed_mutex> lck(ConfMutex);
			pConf->BinaryForcedStates[idx] = true;
		}
		auto event = std::make_shared<EventInfo>(EventType::Binary,idx,Name,QualityFlags::ONLINE);
		bool valb = (val >= 1);
		event->SetPayload<EventType::Binary>(std::move(valb));
		PublishEvent(event);
	}
	else if(type == "Analog")
	{
		{ //lock scope
			auto pConf = static_cast<SimPortConf*>(this->pConf.get());
			std::unique_lock<std::shared_timed_mutex> lck(ConfMutex);
			pConf->AnalogForcedStates[idx] = true;
		}
		auto event = std::make_shared<EventInfo>(EventType::Analog,idx,Name,QualityFlags::ONLINE);
		event->SetPayload<EventType::Analog>(std::move(val));
		PublishEvent(event);
	}
	else
		return false;

	return true;
}

bool SimPort::Release(const std::string& type, const std::string& index)
{
	size_t idx;
	try
	{
		idx = std::stoi(index);
	}
	catch(std::invalid_argument e)
	{
		return false;
	}

	if(type == "Binary")
	{
		auto pConf = static_cast<SimPortConf*>(this->pConf.get());
		std::unique_lock<std::shared_timed_mutex> lck(ConfMutex);
		pConf->BinaryForcedStates[idx] = false;
	}
	else if(type == "Analog")
	{
		auto pConf = static_cast<SimPortConf*>(this->pConf.get());
		std::unique_lock<std::shared_timed_mutex> lck(ConfMutex);
		pConf->AnalogForcedStates[idx] = false;
	}
	else
		return false;

	return true;
}

void SimPort::PortUp()
{
	auto pConf = static_cast<SimPortConf*>(this->pConf.get());
	std::shared_lock<std::shared_timed_mutex> lck(ConfMutex);
	for(auto index : pConf->AnalogIndicies)
	{
		//send initial event
		auto mean = pConf->AnalogStartVals.count(index) ? pConf->AnalogStartVals[index] : 0;
		auto event = std::make_shared<EventInfo>(EventType::Analog,index,Name,QualityFlags::ONLINE);
		event->SetPayload<EventType::Analog>(std::move(mean));
		PublishEvent(event);

		//queue up a timer if it has an update interval
		if(pConf->AnalogUpdateIntervalms.count(index))
		{
			auto interval = pConf->AnalogUpdateIntervalms[index];
			auto std_dev = pConf->AnalogStdDevs.count(index) ? pConf->AnalogStdDevs[index] : (mean ? (pConf->default_std_dev_factor*mean) : 20);

			pTimer_t pTimer = pIOS->make_steady_timer();
			Timers.push_back(pTimer);

			//use a heap pointer as a random seed
			auto seed = (rand_t)((intptr_t)(pTimer.get()));

			pTimer->expires_from_now(std::chrono::milliseconds(random_interval(interval, seed)));
			pTimer->async_wait([=](asio::error_code err_code)
				{
					//FIXME: check err_code?
					if(enabled)
						SpawnEvent(index, mean, std_dev, interval, pTimer, seed);
				});
		}
	}
	for(auto index : pConf->BinaryIndicies)
	{
		//send initial event
		auto val = pConf->BinaryStartVals.count(index) ? pConf->BinaryStartVals[index] : false;
		auto event = std::make_shared<EventInfo>(EventType::Binary,index,Name,QualityFlags::ONLINE);
		event->SetPayload<EventType::Binary>(std::move(val));
		PublishEvent(event);

		//queue up a timer if it has an update interval
		if(pConf->BinaryUpdateIntervalms.count(index))
		{
			auto interval = pConf->BinaryUpdateIntervalms[index];

			pTimer_t pTimer = pIOS->make_steady_timer();
			Timers.push_back(pTimer);

			//use a heap pointer as a random seed
			auto seed = (rand_t)((intptr_t)(pTimer.get()));

			pTimer->expires_from_now(std::chrono::milliseconds(random_interval(interval, seed)));
			pTimer->async_wait([=](asio::error_code err_code)
				{
					//FIXME: check err_code?
					if(enabled)
						SpawnEvent(index, val, interval, pTimer, seed);
				});
		}
	}
}

void SimPort::PortDown()
{
	for(auto pTimer : Timers)
		pTimer->cancel();
	Timers.clear();
}

void SimPort::SpawnEvent(size_t index, double mean, double std_dev, unsigned int interval, pTimer_t pTimer, rand_t seed)
{
	//Restart the timer
	pTimer->expires_from_now(std::chrono::milliseconds(random_interval(interval, seed)));

	{ //lock scope
		auto pConf = static_cast<SimPortConf*>(this->pConf.get());
		std::shared_lock<std::shared_timed_mutex> lck(ConfMutex);

		if(!pConf->AnalogForcedStates[index])
		{
			//Send an event out
			//change value around mean
			std::normal_distribution<double> distribution(mean, std_dev);
			double val = distribution(RandNumGenerator);
			auto event = std::make_shared<EventInfo>(EventType::Analog,index,Name,QualityFlags::ONLINE);
			event->SetPayload<EventType::Analog>(std::move(val));
			PublishEvent(event);
		}
	}

	//wait til next time
	pTimer->async_wait([=](asio::error_code err_code)
		{
			//FIXME: check err_code?
			if(enabled)
				SpawnEvent(index,mean,std_dev,interval,pTimer,seed);
			//else - break timer cycle
		});
}

void SimPort::SpawnEvent(size_t index, bool val, unsigned int interval, pTimer_t pTimer, rand_t seed)
{
	//Restart the timer
	pTimer->expires_from_now(std::chrono::milliseconds(random_interval(interval, seed)));

	{ //lock scope
		auto pConf = static_cast<SimPortConf*>(this->pConf.get());
		std::shared_lock<std::shared_timed_mutex> lck(ConfMutex);

		if(!pConf->BinaryForcedStates[index])
		{
			//Send an event out
			auto event = std::make_shared<EventInfo>(EventType::Binary,index,Name,QualityFlags::ONLINE);
			event->SetPayload<EventType::Binary>(std::move(val));
			PublishEvent(event);
		}
	}

	//wait til next time
	pTimer->async_wait([=](asio::error_code err_code)
		{
			//FIXME: check err_code?
			if(enabled)
				SpawnEvent(index,!val,interval,pTimer,seed);
			//else - break timer cycle
		});
}

void SimPort::Build()
{
	pEnableDisableSync = pIOS->make_strand();
	auto shared_this = std::static_pointer_cast<SimPort>(shared_from_this());
	this->SimCollection->Add(shared_this,this->Name);
}

void SimPort::ProcessElements(const Json::Value& JSONRoot)
{
	auto pConf = static_cast<SimPortConf*>(this->pConf.get());
	std::unique_lock<std::shared_timed_mutex> lck(ConfMutex);

	if(JSONRoot.isMember("Analogs"))
	{
		const auto Analogs = JSONRoot["Analogs"];
		for(Json::ArrayIndex n = 0; n < Analogs.size(); ++n)
		{
			size_t start, stop;
			if(Analogs[n].isMember("Index"))
				start = stop = Analogs[n]["Index"].asUInt();
			else if(Analogs[n]["Range"].isMember("Start") && Analogs[n]["Range"].isMember("Stop"))
			{
				start = Analogs[n]["Range"]["Start"].asUInt();
				stop = Analogs[n]["Range"]["Stop"].asUInt();
			}
			else
			{
				if(auto log = odc::spdlog_get("SimPort"))
					log->error("A point needs an \"Index\" or a \"Range\" with a \"Start\" and a \"Stop\" : '{}'", Analogs[n].toStyledString());
				continue;
			}
			for(auto index = start; index <= stop; index++)
			{
				bool exists = false;
				for(auto existing_index : pConf->AnalogIndicies)
					if(existing_index == index)
						exists = true;

				if(!exists)
					pConf->AnalogIndicies.push_back(index);

				if(Analogs[n].isMember("StdDev"))
					pConf->AnalogStdDevs[index] = Analogs[n]["StdDev"].asDouble();
				if(Analogs[n].isMember("UpdateIntervalms"))
					pConf->AnalogUpdateIntervalms[index] = Analogs[n]["UpdateIntervalms"].asUInt();

				if(Analogs[n].isMember("StartVal"))
				{
					std::string start_val = Analogs[n]["StartVal"].asString();
					if(start_val == "D") //delete this index
					{
						if(pConf->AnalogStartVals.count(index))
							pConf->AnalogStartVals.erase(index);
						if(pConf->AnalogStdDevs.count(index))
							pConf->AnalogStdDevs.erase(index);
						if(pConf->AnalogUpdateIntervalms.count(index))
							pConf->AnalogUpdateIntervalms.erase(index);
						for(auto it = pConf->AnalogIndicies.begin(); it != pConf->AnalogIndicies.end(); it++)
							if(*it == index)
							{
								pConf->AnalogIndicies.erase(it);
								break;
							}
					}
					else if(start_val == "NAN" || start_val == "nan" || start_val == "NaN")
					{
						pConf->AnalogStartVals[index] = std::numeric_limits<double>::quiet_NaN();
					}
					else if(start_val == "INF" || start_val == "inf")
					{
						pConf->AnalogStartVals[index] = std::numeric_limits<double>::infinity();
					}
					else if(start_val == "-INF" || start_val == "-inf")
					{
						pConf->AnalogStartVals[index] = -std::numeric_limits<double>::infinity();
					}
					else if(start_val == "X")
						pConf->AnalogStartVals[index] = 0; //TODO: implement quality - use std::pair, or build the EventInfo here
					else
						pConf->AnalogStartVals[index] = std::stod(start_val);
				}
				else if(pConf->AnalogStartVals.count(index))
					pConf->AnalogStartVals.erase(index);
			}
		}
		std::sort(pConf->AnalogIndicies.begin(),pConf->AnalogIndicies.end());
	}

	if(JSONRoot.isMember("Binaries"))
	{
		const auto Binaries = JSONRoot["Binaries"];
		for(Json::ArrayIndex n = 0; n < Binaries.size(); ++n)
		{
			size_t start, stop;
			if(Binaries[n].isMember("Index"))
				start = stop = Binaries[n]["Index"].asUInt();
			else if(Binaries[n]["Range"].isMember("Start") && Binaries[n]["Range"].isMember("Stop"))
			{
				start = Binaries[n]["Range"]["Start"].asUInt();
				stop = Binaries[n]["Range"]["Stop"].asUInt();
			}
			else
			{
				if(auto log = odc::spdlog_get("SimPort"))
					log->error("A point needs an \"Index\" or a \"Range\" with a \"Start\" and a \"Stop\" : '{}'", Binaries[n].toStyledString());
				continue;
			}
			for(auto index = start; index <= stop; index++)
			{

				bool exists = false;
				for(auto existing_index : pConf->BinaryIndicies)
					if(existing_index == index)
						exists = true;

				if(!exists)
					pConf->BinaryIndicies.push_back(index);

				if(Binaries[n].isMember("UpdateIntervalms"))
					pConf->BinaryUpdateIntervalms[index] = Binaries[n]["UpdateIntervalms"].asUInt();

				if(Binaries[n].isMember("StartVal"))
				{
					std::string start_val = Binaries[n]["StartVal"].asString();
					if(start_val == "D") //delete this index
					{
						if(pConf->BinaryStartVals.count(index))
							pConf->BinaryStartVals.erase(index);
						if(pConf->BinaryUpdateIntervalms.count(index))
							pConf->BinaryUpdateIntervalms.erase(index);
						for(auto it = pConf->BinaryIndicies.begin(); it != pConf->BinaryIndicies.end(); it++)
							if(*it == index)
							{
								pConf->BinaryIndicies.erase(it);
								break;
							}
					}
					else if(start_val == "X")
						pConf->BinaryStartVals[index] = false; //TODO: implement quality - use std::pair, or build the EventInfo here
					else
						pConf->BinaryStartVals[index] = Binaries[n]["StartVal"].asBool();
				}
				else if(pConf->BinaryStartVals.count(index))
					pConf->BinaryStartVals.erase(index);
			}
		}
		std::sort(pConf->BinaryIndicies.begin(),pConf->BinaryIndicies.end());
	}

	if(JSONRoot.isMember("BinaryControls"))
	{
		const auto BinaryControls= JSONRoot["BinaryControls"];
		for(Json::ArrayIndex n = 0; n < BinaryControls.size(); ++n)
		{
			size_t start, stop;
			if(BinaryControls[n].isMember("Index"))
				start = stop = BinaryControls[n]["Index"].asUInt();
			else if(BinaryControls[n]["Range"].isMember("Start") && BinaryControls[n]["Range"].isMember("Stop"))
			{
				start = BinaryControls[n]["Range"]["Start"].asUInt();
				stop = BinaryControls[n]["Range"]["Stop"].asUInt();
			}
			else
			{
				if(auto log = odc::spdlog_get("SimPort"))
					log->error("A point needs an \"Index\" or a \"Range\" with a \"Start\" and a \"Stop\" : '{}'", BinaryControls[n].toStyledString());
				continue;
			}
			for(auto index = start; index <= stop; index++)
			{
				bool exists = false;
				for(auto existing_index : pConf->ControlIndicies)
					if(existing_index == index)
						exists = true;

				if(!exists)
					pConf->ControlIndicies.push_back(index);

				if(BinaryControls[n].isMember("Intervalms"))
					pConf->ControlIntervalms[index] = BinaryControls[n]["Intervalms"].asUInt();

				auto start_val = BinaryControls[n]["StartVal"].asString();
				if(start_val == "D")
				{
					if(pConf->ControlIntervalms.count(index))
						pConf->ControlIntervalms.erase(index);
					for(auto it = pConf->ControlIndicies.begin(); it != pConf->ControlIndicies.end(); it++)
						if(*it == index)
						{
							pConf->ControlIndicies.erase(it);
							break;
						}
				}

				if(BinaryControls[n].isMember("FeedbackBinaries"))
				{
					const auto FeedbackBinaries= BinaryControls[n]["FeedbackBinaries"];
					for(Json::ArrayIndex fbn = 0; fbn < FeedbackBinaries.size(); ++fbn)
					{
						if(!FeedbackBinaries[fbn].isMember("Index"))
						{
							if(auto log = odc::spdlog_get("SimPort"))
								log->error("An 'Index' is required for Binary feedback : '{}'",FeedbackBinaries[fbn].toStyledString());
							continue;
						}

						auto fb_index = FeedbackBinaries[fbn]["Index"].asUInt();
						auto on_qual = QualityFlags::ONLINE;
						auto off_qual = QualityFlags::ONLINE;
						bool on_val = true;
						bool off_val = false;
						auto mode = FeedbackMode::LATCH;

						if(FeedbackBinaries[fbn].isMember("OnValue"))
						{
							if(FeedbackBinaries[fbn]["OnValue"].asString() == "X")
								on_qual = QualityFlags::COMM_LOST;
							else
								on_val = FeedbackBinaries[fbn]["OnValue"].asBool();
						}
						if(FeedbackBinaries[fbn].isMember("OffValue"))
						{
							if(FeedbackBinaries[fbn]["OffValue"].asString() == "X")
								off_qual = QualityFlags::COMM_LOST;
							else
								off_val = FeedbackBinaries[fbn]["OffValue"].asBool();

						}
						if(FeedbackBinaries[fbn].isMember("FeedbackMode"))
						{
							auto mode_str = FeedbackBinaries[fbn]["FeedbackMode"].asString();
							if(mode_str == "PULSE")
								mode = FeedbackMode::PULSE;
							else if(mode_str == "LATCH")
								mode = FeedbackMode::LATCH;
							else
							{
								if(auto log = odc::spdlog_get("SimPort"))
									log->warn("Unrecognised feedback mode: '{}'",FeedbackBinaries[fbn].toStyledString());
							}
						}

						auto on = std::make_shared<EventInfo>(EventType::Binary,fb_index,Name,on_qual);
						on->SetPayload<EventType::Binary>(std::move(on_val));
						auto off = std::make_shared<EventInfo>(EventType::Binary,fb_index,Name,off_qual);
						off->SetPayload<EventType::Binary>(std::move(off_val));

						pConf->ControlFeedback[index].emplace_back(on,off,mode);
					}
				}
			}
		}
		std::sort(pConf->ControlIndicies.begin(),pConf->ControlIndicies.end());
	}
}

void SimPort::Event(std::shared_ptr<const EventInfo> event, const std::string& SenderName, SharedStatusCallback_t pStatusCallback)
{
	if(auto log = odc::spdlog_get("SimPort"))
		log->trace("{}: Recieved control.", Name);
	if(event->GetEventType() != EventType::ControlRelayOutputBlock)
	{
		if(auto log = odc::spdlog_get("SimPort"))
			log->trace("{}: Control code not supported.", Name);
		(*pStatusCallback)(CommandStatus::NOT_SUPPORTED);
		return;
	}
	auto index = event->GetIndex();
	auto& command = event->GetPayload<EventType::ControlRelayOutputBlock>();
	auto pConf = static_cast<SimPortConf*>(this->pConf.get());
	std::shared_lock<std::shared_timed_mutex> lck(ConfMutex);
	for(auto i : pConf->ControlIndicies)
	{
		if(i == index)
		{
			if(auto log = odc::spdlog_get("SimPort"))
				log->trace("{}: Control {}: Matched configured index.", Name, index);
			if(pConf->ControlFeedback.count(i))
			{
				if(auto log = odc::spdlog_get("SimPort"))
					log->trace("{}: Control {}: Setting ({}) control feedback point(s)...", Name, index, pConf->ControlFeedback[i].size());
				for(auto& fb : pConf->ControlFeedback[i])
				{
					if(fb.mode == FeedbackMode::PULSE)
					{
						if(auto log = odc::spdlog_get("SimPort"))
							log->trace("{}: Control {}: Pulse feedback to Binary {}.", Name, index,fb.on_value->GetIndex());
						switch(command.functionCode)
						{
							case ControlCode::PULSE_ON:
							case ControlCode::LATCH_ON:
							case ControlCode::LATCH_OFF:
							case ControlCode::CLOSE_PULSE_ON:
							case ControlCode::TRIP_PULSE_ON:
							{
								PublishEvent(fb.on_value);
								pTimer_t pTimer = pIOS->make_steady_timer();
								pTimer->expires_from_now(std::chrono::milliseconds(command.onTimeMS));
								pTimer->async_wait([pTimer,fb,this](asio::error_code err_code)
									{
										//FIXME: check err_code?
										PublishEvent(fb.off_value);
									});
								//TODO: (maybe) implement multiple pulses - command has count and offTimeMS
								break;
							}
							default:
								(*pStatusCallback)(CommandStatus::NOT_SUPPORTED);
								return;
						}
					}
					else //LATCH
					{
						switch(command.functionCode)
						{
							case ControlCode::LATCH_ON:
							case ControlCode::CLOSE_PULSE_ON:
							case ControlCode::PULSE_ON:
								if(auto log = odc::spdlog_get("SimPort"))
									log->trace("{}: Control {}: Latch on feedback to Binary {}.",
										Name, index,fb.on_value->GetIndex());
								fb.on_value->SetTimestamp();
								PublishEvent(fb.on_value);
								break;
							case ControlCode::LATCH_OFF:
							case ControlCode::TRIP_PULSE_ON:
							case ControlCode::PULSE_OFF:
								if(auto log = odc::spdlog_get("SimPort"))
									log->trace("{}: Control {}: Latch off feedback to Binary {}.",
										Name, index,fb.off_value->GetIndex());
								fb.off_value->SetTimestamp();
								PublishEvent(fb.off_value);
								break;
							default:
								(*pStatusCallback)(CommandStatus::NOT_SUPPORTED);
								return;
						}
					}
				}
				(*pStatusCallback)(CommandStatus::SUCCESS);
				return;
			}
			else
			{
				if(auto log = odc::spdlog_get("SimPort"))
					log->trace("{}: Control {}: No feeback points configured.", Name, index);
			}
			(*pStatusCallback)(CommandStatus::UNDEFINED);
			return;
		}
	}
	(*pStatusCallback)(CommandStatus::NOT_SUPPORTED);
}
