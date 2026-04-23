//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "provider.h"

namespace cfg
{
	namespace vhost
	{
		namespace app
		{
			namespace pvd
			{
				struct RtspPullProvider : public Provider
				{
					ProviderType GetType() const override
					{
						return ProviderType::RtspPull;
					}

					CFG_DECLARE_CONST_REF_GETTER_OF(IsBlockDuplicateStreamName, _is_block_duplicate_stream_name)
					CFG_DECLARE_CONST_REF_GETTER_OF(IsAcceptH265, _accept_h265)

				protected:
					void MakeList() override
					{
						Provider::MakeList();

						Register<Optional>("BlockDuplicateStreamName", &_is_block_duplicate_stream_name);
						Register<Optional>("AcceptH265", &_accept_h265);
					}

					// true: block(disconnect) new incoming stream
					// false: don't block new incoming stream
					bool _is_block_duplicate_stream_name = true;
					// true: accept H.265/HEVC streams from RTSP pull
					// false: reject H.265/HEVC streams (default, for backward compatibility)
					bool _accept_h265 = false;
				};
			}  // namespace pvd
		}  // namespace app
	}  // namespace vhost
}  // namespace cfg