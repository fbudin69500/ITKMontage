/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#ifndef itkMaxPhaseCorrelationOptimizer_hxx
#define itkMaxPhaseCorrelationOptimizer_hxx

#include "itkMaxPhaseCorrelationOptimizer.h"

#include "itkImageRegionConstIterator.h"
#include "itkImageRegionIteratorWithIndex.h"

#include <cmath>
#include <type_traits>

/*
 * \author Jakub Bican, jakub.bican@matfyz.cz, Department of Image Processing,
 *         Institute of Information Theory and Automation,
 *         Academy of Sciences of the Czech Republic.
 *
 */

namespace itk
{
template< typename TRegistrationMethod >
void
MaxPhaseCorrelationOptimizer< TRegistrationMethod >
::PrintSelf( std::ostream& os, Indent indent ) const
{
  Superclass::PrintSelf( os, indent );
  os << indent << "MaxCalculator: " << m_MaxCalculator << std::endl;
  auto pim = static_cast< typename std::underlying_type< PeakInterpolationMethod >::type >( m_PeakInterpolationMethod );
  os << indent << "PeakInterpolationMethod: " << pim << std::endl;
  os << indent << "ZeroSuppression: " << m_ZeroSuppression << std::endl;
  os << indent << "BiasTowardsExpected: " << m_BiasTowardsExpected << std::endl;
}

template< typename TRegistrationMethod >
void
MaxPhaseCorrelationOptimizer< TRegistrationMethod >
::SetPeakInterpolationMethod( const PeakInterpolationMethod peakInterpolationMethod )
{
  if ( this->m_PeakInterpolationMethod != peakInterpolationMethod )
    {
    this->m_PeakInterpolationMethod = peakInterpolationMethod;
    this->Modified();
    }
}

template< typename TRegistrationMethod >
void
MaxPhaseCorrelationOptimizer< TRegistrationMethod >
::ComputeOffset()
{
  ImageConstPointer input = static_cast< ImageType* >( this->GetInput( 0 ) );
  ImageConstPointer fixed = static_cast< ImageType* >( this->GetInput( 1 ) );
  ImageConstPointer moving = static_cast< ImageType* >( this->GetInput( 2 ) );

  OffsetType offset;
  offset.Fill( 0 );

  if ( !input )
    {
    return;
    }

  const typename ImageType::RegionType wholeImage = input->GetLargestPossibleRegion();
  const typename ImageType::SizeType size = wholeImage.GetSize();
  const typename ImageType::IndexType oIndex = wholeImage.GetIndex();

  const typename ImageType::SpacingType spacing = input->GetSpacing();
  const typename ImageType::PointType fixedOrigin = fixed->GetOrigin();
  const typename ImageType::PointType movingOrigin = moving->GetOrigin();

  // create the image which be biased towards the expected solution and be zero-suppressed
  typename ImageType::Pointer iAdjusted = ImageType::New();
  iAdjusted->CopyInformation( input );
  iAdjusted->SetRegions( input->GetBufferedRegion() );
  iAdjusted->Allocate( false );

  typename ImageType::IndexType adjustedSize;
  typename ImageType::IndexType directExpectedIndex;
  typename ImageType::IndexType mirrorExpectedIndex;
  double distancePenaltyFactor = 0.0;
  for ( unsigned d = 0; d < ImageDimension; d++ )
    {
    adjustedSize[d] = size[d] + oIndex[d];
    distancePenaltyFactor += adjustedSize[d] * adjustedSize[d]; // make it proportional to image size
    directExpectedIndex[d] = ( movingOrigin[d] - fixedOrigin[d] ) / spacing[d] + oIndex[d];
    mirrorExpectedIndex[d] = ( movingOrigin[d] - fixedOrigin[d] ) / spacing[d] + adjustedSize[d];
    }

  distancePenaltyFactor = -m_BiasTowardsExpected / distancePenaltyFactor;

  MultiThreaderBase* mt = this->GetMultiThreader();
  mt->ParallelizeImageRegion< ImageDimension >( wholeImage,
    [&](const typename ImageType::RegionType & region)
    {
      ImageRegionConstIterator< ImageType > iIt(input, region);
      ImageRegionIteratorWithIndex< ImageType > oIt(iAdjusted, region);
      for (; !oIt.IsAtEnd(); ++iIt, ++oIt)
        {
        typename ImageType::IndexType ind = oIt.GetIndex();
        IndexValueType dist = 0;
        for (unsigned d = 0; d < ImageDimension; d++)
          {
          IndexValueType distDirect = ( directExpectedIndex[d] - ind[d] ) * ( directExpectedIndex[d] - ind[d] );
          IndexValueType distMirror = ( mirrorExpectedIndex[d] - ind[d] ) * ( mirrorExpectedIndex[d] - ind[d] );
          if ( distDirect <= distMirror )
            {
            dist += distDirect;
            }
          else
            {
            dist += distMirror;
            }
          }

        typename ImageType::PixelType pixel = iIt.Get() * std::exp( distancePenaltyFactor * dist );
#ifndef NDEBUG
        pixel *= 1000; // make the intensities in this image more humane (close to 1.0)
        // it is really hard to count zeroes after decimal point when comparing pixel intensities
        // since this images is used to find maxima, absolute values are irrelevant
#endif
        oIt.Set( pixel );
        }
    },
    nullptr );

  WriteDebug( iAdjusted.GetPointer(), "iAdjusted.nrrd" );

  // suppress trivial zero solution
  FixedArray< double, ImageDimension > dimFactor; // each dimension might have different size
  for ( unsigned d = 0; d < ImageDimension; d++ )
    {
    dimFactor = 100.0 / size[d]; // turn absolute size into percentages
    }
  constexpr IndexValueType znSize = 4; // zero neighborhood size, in city-block distance
  mt->ParallelizeImageRegion<ImageDimension>( wholeImage,
    [&]( const typename ImageType::RegionType& region )
    {
      ImageRegionIteratorWithIndex< ImageType > oIt(iAdjusted, region);
      for (; !oIt.IsAtEnd(); ++oIt)
        {
        bool pixelValid = false;
        typename ImageType::PixelType pixel;
        typename ImageType::IndexType ind = oIt.GetIndex();
        IndexValueType dist = 0;
        for ( unsigned d = 0; d < ImageDimension; d++ )
          {
          dist += ind[d] - oIndex[d];
          }
        if ( dist < znSize ) // neighborhood of [0,0,...,0] - in case zero peak is blurred
          {
          pixel = oIt.Get();
          // avoid the initial steep rise of function x/(1+x) by shifting it by 3
          pixel *= ( dist + 3 ) / ( m_ZeroSuppression + dist + 3 );
          pixelValid = true;
          }

        for ( unsigned d = 0; d < ImageDimension; d++ ) // lines/sheets of zero indices
          {
          if ( ind[d] == oIndex[d] ) // one of the indices is "zero"
            {
            if ( !pixelValid )
              {
              pixel = oIt.Get();
              pixelValid = true;
              }
            IndexValueType distD = ind[d] - oIndex[d];
            if ( distD > IndexValueType( size[d] / 2 ) ) // wrap around
              {
              distD = size[d] - distD;
              }
            double distF = distD * dimFactor[d];
            // avoid the initial steep rise of x/(1+x) by shifting it by 3% of image size
            pixel *= ( distF + 3 ) / ( m_ZeroSuppression + distF + 3 );
            }
          }

        if ( pixelValid ) // either neighborhood or lines/sheets has updated the pixel
          {
          oIt.Set( pixel );
          }
        }
    },
    nullptr );

  WriteDebug( iAdjusted.GetPointer(), "iAdjustedZS.nrrd" );

  m_MaxCalculator->SetImage( iAdjusted );
  m_MaxCalculator->SetN( std::ceil( this->m_Offsets.size() / 2 ) *
                         ( static_cast< unsigned >( std::pow( 3, ImageDimension ) ) - 1 ) );

  try
    {
    m_MaxCalculator->ComputeMaxima();
    }
  catch ( ExceptionObject& err )
    {
    itkDebugMacro( "exception caught during execution of max calculator - passing " );
    throw err;
    }

  this->m_Confidences = m_MaxCalculator->GetMaxima();
  typename MaxCalculatorType::IndexVector indices = m_MaxCalculator->GetIndicesOfMaxima();
  itkAssertOrThrowMacro( this->m_Confidences.size() == indices.size(),
      "Maxima and their indices must have the same number of elements" );
  std::greater< PixelType > compGreater;
  auto zeroBound = std::upper_bound( this->m_Confidences.begin(), this->m_Confidences.end(), 0.0, compGreater );
  if ( zeroBound != this->m_Confidences.end() ) // there are some non-positive values in here
    {
    unsigned i = zeroBound - this->m_Confidences.begin();
    this->m_Confidences.resize( i );
    indices.resize( i );
    }

  if ( m_MergePeaks > 0 ) // eliminate indices belonging to the same blurry peak
    {
    unsigned i = 1;
    while ( i < indices.size() )
      {
      unsigned k = 0;
      while ( k < i )
        {
        // calculate maximum distance along any dimension
        SizeValueType dist = 0;
        for ( unsigned d = 0; d < ImageDimension; d++ )
          {
          SizeValueType d1 = std::abs( indices[i][d] - indices[k][d] );
          if ( d1 > size[d] / 2 ) // wrap around
            {
            d1 = size[d] - d1;
            }
          dist = std::max( dist, d1 );
          }
        if ( dist < 2 ) // for city-block this is equivalent to:  dist == 1
          {
          break;
          }
        ++k;
        }

      if ( k < i ) // k is nearby
        {
        this->m_Confidences[k] += this->m_Confidences[i]; // join amplitudes
        this->m_Confidences.erase( this->m_Confidences.begin() + i );
        indices.erase( indices.begin() + i );
        }
      else // examine next index
        {
        ++i;
        }
      }

    // now we need to re-sort the values
    std::vector< unsigned > sIndices;
    sIndices.reserve( this->m_Confidences.size() );
    for ( i = 0; i < this->m_Confidences.size(); i++ )
      {
      sIndices.push_back( i );
      }
    std::sort( sIndices.begin(), sIndices.end(),
      [this]( unsigned a, unsigned b )
      {
        return this->m_Confidences[a] > this->m_Confidences[b];
      }
    );

    // now apply sorted order
    typename MaxCalculatorType::ValueVector tMaxs( this->m_Confidences.size() );
    typename MaxCalculatorType::IndexVector tIndices( this->m_Confidences.size() );
    for ( i = 0; i < this->m_Confidences.size(); i++ )
      {
      tMaxs[i] = this->m_Confidences[sIndices[i]];
      tIndices[i] = indices[sIndices[i]];
      }
    this->m_Confidences.swap( tMaxs );
    indices.swap( tIndices );
    }

  if ( this->m_Offsets.size() > this->m_Confidences.size() )
    {
    this->SetOffsetCount( this->m_Confidences.size() );
    }
  else
    {
    this->m_Confidences.resize( this->m_Offsets.size() );
    indices.resize( this->m_Offsets.size() );
    }

  double confidenceFactor = 1.0 / this->m_Confidences[0];

  for ( unsigned m = 0; m < this->m_Confidences.size(); m++ )
    {
    using ContinuousIndexType = ContinuousIndex< OffsetScalarType, ImageDimension >;
    ContinuousIndexType maxIndex = indices[m];

    if ( m_PeakInterpolationMethod != PeakInterpolationMethod::None ) // interpolate the peak
      {
      typename ImageType::PixelType y0, y1 = this->m_Confidences[m], y2;
      typename ImageType::IndexType tempIndex = indices[m];

      for ( unsigned i = 0; i < ImageDimension; i++ )
        {
        tempIndex[i] = maxIndex[i] - 1;
        if ( !wholeImage.IsInside( tempIndex ) )
          {
          tempIndex[i] = maxIndex[i];
          continue;
          }
        y0 = iAdjusted->GetPixel( tempIndex );
        tempIndex[i] = maxIndex[i] + 1;
        if ( !wholeImage.IsInside( tempIndex ) )
          {
          tempIndex[i] = maxIndex[i];
          continue;
          }
        y2 = iAdjusted->GetPixel( tempIndex );
        tempIndex[i] = maxIndex[i];

        OffsetScalarType omega, theta;
        switch ( m_PeakInterpolationMethod )
          {
          case PeakInterpolationMethod::Parabolic:
            maxIndex[i] += ( y0 - y2 ) / ( 2 * ( y0 - 2 * y1 + y2 ) );
            break;
          case PeakInterpolationMethod::Cosine:
            omega = std::acos( ( y0 + y2 ) / ( 2 * y1 ) );
            theta = std::atan( ( y0 - y2 ) / ( 2 * y1 * std::sin( omega ) ) );
            maxIndex[i] -= ::itk::Math::one_over_pi * theta / omega;
            break;
          default:
            itkAssertInDebugAndIgnoreInReleaseMacro( "Unknown interpolation method" );
            break;
          } // switch PeakInterpolationMethod
        } // for ImageDimension
      } // if Interpolation != None

    for ( unsigned i = 0; i < ImageDimension; ++i )
      {
      OffsetScalarType directOffset = ( movingOrigin[i] - fixedOrigin[i] )
        - 1 * spacing[i] * ( maxIndex[i] - oIndex[i] );
      OffsetScalarType mirrorOffset = ( movingOrigin[i] - fixedOrigin[i] )
        - 1 * spacing[i] * ( maxIndex[i] - adjustedSize[i] );
      if ( std::abs( directOffset ) <= std::abs( mirrorOffset ) )
        {
        offset[i] = directOffset;
        }
      else
        {
        offset[i] = mirrorOffset;
        }
      }

    //this->m_Confidences[m] *= confidenceFactor; // normalize - highest confidence will be 1.0
#ifdef NDEBUG
    this->m_Confidences[m] *= 1000.0; // make the intensities more humane (close to 1.0)
#endif
    
    this->m_Offsets[m] = offset;
    }
}

} // end namespace itk

#endif
