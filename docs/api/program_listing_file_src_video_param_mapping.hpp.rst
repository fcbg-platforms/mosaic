
.. _program_listing_file_src_video_param_mapping.hpp:

Program Listing for File param_mapping.hpp
==========================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_param_mapping.hpp>` (``src\video\param_mapping.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <algorithm>
   #include <cmath>
   #include <cstdint>
   
   namespace mosaic {
   
   // Rounds a UI double onto an integer GenICam node's device range. Pure
   // function (no Pylon dependency) so it's directly unit-testable — used by
   // VideoGrabber::apply_image_params() for BlackLevel→BlackLevelRaw and
   // DigitalShift.
   [[nodiscard]] inline int64_t round_clamp_to_int_range(double value,
                                                          int64_t nodeMin,
                                                          int64_t nodeMax) {
       const auto v = static_cast<int64_t>(std::lround(value));
       return std::clamp(v, nodeMin, nodeMax);
   }
   
   // Maps a normalized [0.0, 1.0] UI value onto an integer GenICam node's
   // device range. Pure function — used by apply_image_params() for
   // AutoTargetBrightness→AutoTargetValue.
   [[nodiscard]] inline int64_t map_normalized_to_int_range(double normalized,
                                                             int64_t nodeMin,
                                                             int64_t nodeMax) {
       const double norm = std::clamp(normalized, 0.0, 1.0);
       const auto v = static_cast<int64_t>(
           std::lround(static_cast<double>(nodeMin) +
                       norm * static_cast<double>(nodeMax - nodeMin)));
       return std::clamp(v, nodeMin, nodeMax);
   }
   
   } // namespace mosaic
