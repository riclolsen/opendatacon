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
//
//  IJsonResponder.h
//  opendatacon
//
//  Created by Alan Murray on 29/08/2014.
//  
//

#ifndef opendatacon_IJsonResponder_h
#define opendatacon_IJsonResponder_h

#include <json/json.h>
#include <unordered_map>

typedef std::unordered_map<std::string, std::string> ParamCollection;

class IJsonResponder
{
public:
    virtual Json::Value GetResponse(const ParamCollection& params) const
    {
        Json::Value event;
        
        event["Configuration"] = GetConfiguration(params);
        event["CurrentState"] = GetCurrentState(params);
        event["Statistics"] = GetStatistics(params);
        
        return event;
    };

    virtual Json::Value GetStatistics(const ParamCollection& params) const
    {
        return Json::Value();
    };
    
    virtual Json::Value GetCurrentState(const ParamCollection& params) const
    {
        return Json::Value();
    };

    virtual Json::Value GetConfiguration(const ParamCollection& params) const
    {
        return Json::Value();
    };
};

#endif
