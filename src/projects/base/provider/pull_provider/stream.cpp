//==============================================================================
//
//  PullProvider Base Class
//
//  Created by Getroot
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================

#include "stream.h"

#include "application.h"
#include "base/info/application.h"
#include "provider_private.h"
#include "stream_props.h"

#include <chrono>
#include <thread>

namespace pvd
{
	PullStream::PullStream(const std::shared_ptr<pvd::Application> &application, const info::Stream &stream_info, const std::vector<ov::String> &url_list, const std::shared_ptr<pvd::PullStreamProperties> &properties)
		: Stream(application, stream_info)
	{
		for (auto &url : url_list)
		{
			auto parsed_url = ov::Url::Parse(url);
			if (parsed_url)
			{
				_url_list.push_back(parsed_url);
			}
		}

		// In case of Pull Stream created by Origins, Properties information is included.
		_properties = properties;
		if (_properties == nullptr)
		{
			_properties = std::make_shared<pvd::PullStreamProperties>();
		}
		
		SetRepresentationType((_properties->IsRelay()==true)?StreamRepresentationType::Relay:StreamRepresentationType::Source);

		_from_origin_map_store = _properties->IsFromOriginMapStore();
	}

	bool PullStream::Start()
	{
		std::lock_guard<std::mutex> lock(_start_stop_stream_lock);
		_restart_count = 0;
		const bool is_persistent = (_properties != nullptr) && _properties->IsPersistent();
		while (true)
		{
			if (StartStream(GetNextURL()) == false)
			{
				_restart_count++;
				// For non-persistent streams, respect retry budget and terminate.
				// For persistent streams, never enter TERMINATED due to retry exhaustion;
				// keep the stream registered so it can recover when the origin returns.
				if ((!is_persistent) && (_restart_count > (_url_list.size() * _properties->GetRetryCount())))
				{
					SetState(Stream::State::TERMINATED);
					return false;
				}

				if (is_persistent)
				{
					// Avoid a busy-loop when the origin is down.
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			}
			else
			{
				_restart_count = 0;
				break;
			}
		}

		logti("%s has started to play [%s(%u)] stream : %s", GetApplicationTypeName(), GetName().CStr(), GetId(), GetMediaSource().CStr());
		return Stream::Start();
	}

	bool PullStream::Stop()
	{
		std::lock_guard<std::mutex> lock(_start_stop_stream_lock);
		StopStream();
		return Stream::Stop();
	}

	bool PullStream::Resume()
	{
		const bool is_persistent = (_properties != nullptr) && _properties->IsPersistent();
		if ((_properties->GetRetryCount() <= 0) && (!is_persistent))
		{
			SetState(Stream::State::TERMINATED);
			return false;
		}
		
		if (RestartStream(GetNextURL()) == false)
		{
			Stop();
			_restart_count++;
			if ((!is_persistent) && (_restart_count > _url_list.size() * _properties->GetRetryCount()))
			{
				// If the stream state is TERMINATED, it will be deleted by the StreamMotor
				SetState(Stream::State::TERMINATED);
			}
			else if (is_persistent)
			{
				// For persistent streams keep retrying. Ensure the stream does not get
				// deleted by collector logic that removes TERMINATED streams.
				SetState(Stream::State::ERROR);
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}

			return false;
		}

		UpdateStream();

		_restart_count = 0;
		return Stream::Start();
	}

	const std::shared_ptr<const ov::Url> PullStream::GetNextURL()
	{
		if (_url_list.size() == 0)
		{
			return nullptr;
		}

		if (static_cast<size_t>(_curr_url_index + 1) > _url_list.size())
		{
			_curr_url_index = 0;
		}

		auto curr_url = _url_list[_curr_url_index];
		SetMediaSource(curr_url->ToUrlString(true));

		_curr_url_index++;

		return curr_url;
	}

	const std::shared_ptr<const ov::Url> PullStream::GetPrimaryURL()
	{
		if (_url_list.size() == 0)
		{
			return nullptr;
		}

		auto curr_url = _url_list[0];

		return curr_url;
	}

	bool PullStream::IsCurrPrimaryURL()
	{
		// If the currently playing URL and the 0th URL are the same, it is the primary URL.
		if (GetMediaSource() == _url_list[0]->ToUrlString(true))
			return true;

		return false;
	}

	void PullStream::ResetUrlIndex()
	{
		_curr_url_index = 0;
	}

	std::shared_ptr<pvd::PullStreamProperties> PullStream::GetProperties()
	{
		return _properties;
	}

}  // namespace pvd
