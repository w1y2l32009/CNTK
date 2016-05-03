#pragma once

// the header files located in Source\Multiverso\include
#include <multiverso/multiverso.h>
#include <multiverso/table/matrix_table.h>
#include <multiverso/util/configure.h>

#pragma comment(lib, "Multiverso.lib")

#ifndef CPUONLY
#include <cuda_runtime.h>
#pragma comment (lib, "cudart.lib")     // for cudaMemcpyAsync()
#endif


#include "MPIWrapper.h"
#include "ComputationNetwork.h"
#include "TimerUtility.h"

#include <functional>
#include <thread>
#include <unordered_map>
#include <numeric>

namespace Microsoft {
	namespace MSR {
		namespace CNTK {

#ifndef CPUONLY
#define CudaErrorCheck(ans) { gpuAssert((ans), __FILE__, __LINE__); }
			inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort = true)
			{
				if (code != cudaSuccess)
				{
					fprintf(stderr, "GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
					if (abort) exit(code);
				}
			}
#endif

			enum class AdjustLearningRateatBeginning : int
			{
				None = 0,
				Linearly = 1,
				Staircase = (1 << 1),
			};

			template<class ElemType = float>
			class MultiversoHelper
			{
				typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;
			public:
				MultiversoHelper(const std::list<ComputationNodeBasePtr> & learnableNodes,
					int MPINodeNum,
					bool isAsyncBuffered = true,
					AdjustLearningRateatBeginning adjusttype = AdjustLearningRateatBeginning::None,
					double adjustcoef = 0.2,
					size_t adjustnbmb = 600,
					int traceLevel = 0)
				{
					m_modelSyncCount = 0;
					m_adjustLearningRateAtBeginningType = adjusttype;
					m_adjustCoefficient = adjustcoef;
					m_adjustMBNumber = adjustnbmb;

					m_totalClientNumber = MPINodeNum;

					//Pipeline releated variables
					m_isUseAsyncBuffered = isAsyncBuffered;
					m_localCacheNumber = m_isUseAsyncBuffered ? 2 : 1;
					m_cacheSwapIndex = new int[m_localCacheNumber];

					//CPU asynchronous buffer
					m_cpuAsyncBuffer = new ElemType*[m_localCacheNumber];
#ifndef CPUONLY
					//GPU asynchronous buffer
					//m_gpuAsyncBuffer = new Matrix<ElemType>**[m_localCacheNumber];
          m_gpuAsyncBuffer.resize(m_localCacheNumber);

					//creat an communication stream for the data tranfer between GPU and CPU
					CudaErrorCheck(cudaStreamCreate(&_commStream));
#endif
					m_bufferInUse = 0;
					for (int i = 0; i < m_localCacheNumber; i++)
						m_cacheSwapIndex[i] = (i + 1) % m_localCacheNumber;

					m_prefetchThread = nullptr;

					if (traceLevel > 3)
						multiverso::Log::ResetLogLevel(multiverso::LogLevel::Debug);

					MultiversoInit(learnableNodes);
				}

				~MultiversoHelper()
				{
					fprintf(stderr, "~MultiversoHelper\n");
					fflush(stderr);

					if (m_isUseAsyncBuffered && m_prefetchThread != nullptr && m_prefetchThread->joinable())
						m_prefetchThread->join();

					delete m_cacheSwapIndex, m_deltaArray;

					for (size_t i = 0; i < m_localCacheNumber; i++)
					{
#ifndef CPUONLY
						CudaErrorCheck(cudaFreeHost(m_cpuAsyncBuffer[i]));
#else
						delete m_cpuAsyncBuffer[i];
#endif
					}
					delete m_cpuAsyncBuffer;
#ifndef CPUONLY
					CudaErrorCheck(cudaStreamDestroy(_commStream));
#endif
					multiverso::MV_ShutDown(false);
				}

				// upoload preCompute model to the parameter servers
				void InitModel(const std::list<ComputationNodeBasePtr> & learnableNodes)
				{
					float factor = (float) 1.0 / m_totalClientNumber;

					int i = 0; // indicate the index of learnable nodes
					for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
					{
						ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
						Matrix<ElemType> &mat = node->Value();
#pragma warning( push )
#pragma warning( disable : 4238)

#ifndef CPUONLY
						for (int j = 0; j < m_localCacheNumber; j++)
              m_gpuAsyncBuffer[j].push_back(mat.DeepClone());
#endif
#pragma warning( pop )
						ElemType* px = m_cpuAsyncBuffer[0] + m_tableOffsets[i];
						mat.CopyToArray(px, m_tableLength[i]);
					}

					for (int i = 1; i < m_localCacheNumber; i++)
						memcpy(m_cpuAsyncBuffer[i], m_cpuAsyncBuffer[0], sizeof(ElemType) * m_totalModelSize);

					memcpy(m_deltaArray, m_cpuAsyncBuffer[0], sizeof(ElemType) * m_totalModelSize);

					std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));

					for (int widx = 0; widx < m_tableCount; widx++)
					{
						auto multiversoMatrix = m_matrixArray->at(widx);
						ElemType* px = m_deltaArray + m_tableOffsets[widx];
						multiversoMatrix->Add(px, m_tableLength[widx]);
				}
					WaitAll(); // initial model for every client should be identical
					for (int widx = 0; widx < m_tableCount; widx++)
					{
						auto multiversoMatrix = m_matrixArray->at(widx);
						ElemType* px = m_deltaArray + m_tableOffsets[widx];
						multiversoMatrix->Get(px, m_tableLength[widx]);
					}
				}

				//ASGD logic
				void PushAndPullModel(const std::list<ComputationNodeBasePtr> & learnableNodes)
				{
					//Note: maybe overflow.
					m_modelSyncCount++;

					Timer timer;
					WaitAsyncBuffer();

					m_bufferInUse = m_cacheSwapIndex[m_bufferInUse];

					int i = 0; // indicate the index of learnable nodes
					if (m_isUseAsyncBuffered)
					{

						for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
						{
							ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
							Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();
#ifndef CPUONLY
							//CNTK model -> GPU buffer
							CudaErrorCheck(cudaMemcpy(m_gpuAsyncBuffer[m_bufferInUse][i].Data(),
								mat.Data(),
								mat.GetNumElements() * sizeof(ElemType),
								cudaMemcpyDeviceToDevice));

							//GPU buffer -> CNTK model
							CudaErrorCheck(cudaMemcpy(mat.Data(),
								m_gpuAsyncBuffer[m_cacheSwapIndex[m_bufferInUse]][i].Data(),
								mat.GetNumElements() * sizeof(ElemType),
								cudaMemcpyDeviceToDevice));
#else
							ElemType * px = m_cpuAsyncBuffer[m_bufferInUse] + m_tableOffsets[i];

							mat.CopyToArray(px, m_tableLength[i]);

							ElemType * py = m_cpuAsyncBuffer[m_cacheSwapIndex[m_bufferInUse]] + m_tableOffsets[i];

							mat.SetValue(mat.GetNumRows(), mat.GetNumCols(), mat.GetDeviceId(), py);


							delete px;
#endif
						}
#ifndef CPUONLY
						m_prefetchThread = new thread([&](){
							float factor = DecayCoefficient();
							int t_cacheIdx = m_bufferInUse;
							int deviceId = m_gpuAsyncBuffer[t_cacheIdx][0].GetDeviceId();

							CudaErrorCheck(cudaSetDevice(deviceId));

							for (int widx = 0; widx < m_tableCount; widx++)
							{
								ElemType * px = m_deltaArray + m_tableOffsets[widx];
								//GPU buffer -> CPU buffer
								CudaErrorCheck(cudaMemcpyAsync(px,
									m_gpuAsyncBuffer[t_cacheIdx][widx].Data(),
									m_gpuAsyncBuffer[t_cacheIdx][widx].GetNumElements() * sizeof(ElemType),
									cudaMemcpyDeviceToHost,
									_commStream));
							}

							// waiting copy from GPU to CPU has finished
							CudaErrorCheck(cudaStreamSynchronize(_commStream));

							// delta =  gradient * learning_rate
              std::transform(m_cpuAsyncBuffer[t_cacheIdx], m_cpuAsyncBuffer[t_cacheIdx] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());

							// lr decay
							std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
							for (int widx = 0; widx < m_tableCount; widx++)
							{
								auto multiversoMatrix = m_matrixArray->at(widx);
								ElemType* px = m_deltaArray + m_tableOffsets[widx];
								ElemType* py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];
								multiversoMatrix->Add(px, m_tableLength[widx]);
								multiversoMatrix->Get(py, m_tableLength[widx]);
							}

							// copy parameters from CPU buffer to GPU buffer
							for (int widx = 0; widx < m_tableCount; widx++)
							{
								ElemType * py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];

								CudaErrorCheck(cudaMemcpyAsync(m_gpuAsyncBuffer[t_cacheIdx][widx].Data(),
									py,
									m_gpuAsyncBuffer[t_cacheIdx][widx].GetNumElements() * sizeof(ElemType),
									cudaMemcpyHostToDevice,
									_commStream));
							}

							CudaErrorCheck(cudaStreamSynchronize(_commStream));

						});
#else
						m_prefetchThread = new thread([&](){
							float factor = DecayCoefficient();
							int t_cacheIdx = m_bufferInUse;

              std::transform(m_cpuAsyncBuffer[t_cacheIdx], m_cpuAsyncBuffer[t_cacheIdx] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());
							std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
							for (int widx = 0; widx < m_tableCount; widx++)
							{
								auto multiversoMatrix = m_matrixArray->at(widx);
								ElemType* px = m_deltaArray + m_tableOffsets[widx];
								ElemType* py = m_cpuAsyncBuffer[t_cacheIdx] + m_tableOffsets[widx];
								multiversoMatrix->Add(px, m_tableLength[widx]);
								multiversoMatrix->Get(py, m_tableLength[widx]);
							}
							//m_matrixArray->Add(m_deltaArray, m_totalModelSize);
							//m_matrixArray->Get(m_cpuAsyncBuffer[t_cacheIdx], m_totalModelSize);

						});
#endif
					}
					else
					{
						float factor = DecayCoefficient();
						i = 0;
						for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
						{
							ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
							Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();

							ElemType * px = m_deltaArray + m_tableOffsets[i];
							mat.CopyToArray(px, m_tableLength[i]);
						}

            std::transform(m_cpuAsyncBuffer[0], m_cpuAsyncBuffer[0] + m_totalModelSize, m_deltaArray, m_deltaArray, std::minus<ElemType>());

						// lr decay
						std::transform(m_deltaArray, m_deltaArray + m_totalModelSize, m_deltaArray, std::bind1st(std::multiplies<ElemType>(), factor));
						for (int widx = 0; widx < m_tableCount; widx++)
						{
							auto multiversoMatrix = m_matrixArray->at(widx);
							ElemType* px = m_deltaArray + m_tableOffsets[widx];
							ElemType* py = m_cpuAsyncBuffer[0] + m_tableOffsets[widx];
							multiversoMatrix->Add(px, m_tableLength[widx]);
							multiversoMatrix->Get(py, m_tableLength[widx]);
						}

						i = 0;
						for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++, i++)
						{
							ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
							Microsoft::MSR::CNTK::Matrix<ElemType> &mat = node->Value();

							ElemType * px = m_cpuAsyncBuffer[0] + m_tableOffsets[i];
							mat.SetValue(mat.GetNumRows(), mat.GetNumCols(), mat.GetDeviceId(), px);
						}
					}
				}

				void PushModel(const std::list<ComputationNodeBasePtr> & learnableNode)
				{

				}

				void PullModel(const std::list<ComputationNodeBasePtr> & learnableNode)
				{

				}

				void WaitAll()
				{
					multiverso::MV_Barrier();
				}

				void WaitAsyncBuffer()
				{
					if (m_prefetchThread != nullptr && m_prefetchThread->joinable())
					{
						m_prefetchThread->join();
						delete m_prefetchThread;
						m_prefetchThread = nullptr;
					}
				}
			private:
				void MultiversoInit(const std::list<ComputationNodeBasePtr> & learnableNodes)
				{
					assert(!m_isInitialized);
					m_isInitialized = true;

					multiverso::MV_Init();
          multiverso::SetCMDFlag<std::string>(std::string("updater_type"), std::string("sgd"));

					m_matrixArray = new std::vector< multiverso::MatrixWorkerTable<ElemType>*>();
					m_serverArray = new std::vector< multiverso::MatrixServerTable<ElemType>*>();
					//weights
					for (auto nodeIter = learnableNodes.begin(); nodeIter != learnableNodes.end(); nodeIter++)
					{
						ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(*nodeIter);
						Matrix<ElemType> &mat = node->Value();
						size_t layerSize = mat.GetNumElements();
						size_t layerRowSize = mat.GetNumRows();
						size_t layerColSize = mat.GetNumCols();

						m_matrixArray->push_back(new multiverso::MatrixWorkerTable<ElemType>(layerRowSize, layerColSize));
						m_serverArray->push_back(new multiverso::MatrixServerTable<ElemType>(layerRowSize, layerColSize));

						m_tableLength.push_back(layerSize);
					}

					m_tableCount = m_tableLength.size();

					// cacluate total of learnable node's size
					m_totalModelSize = accumulate(m_tableLength.begin(), m_tableLength.end(), 0);

					multiverso::MV_Barrier();

					size_t idx = 0;
					for (size_t len : m_tableLength)
					{
						m_tableOffsets.push_back(idx);
						idx += len;
					}

#ifndef CPUONLY
					for (int i = 0; i < m_localCacheNumber; i++)
						m_gpuAsyncBuffer[i].reserve(m_tableCount);

					//create pinned memory
					for (int i = 0; i < m_localCacheNumber; ++i)
						CudaErrorCheck(cudaMallocHost((void **)&m_cpuAsyncBuffer[i], sizeof(ElemType) * (m_totalModelSize), cudaHostAllocPortable));

					CudaErrorCheck(cudaMallocHost((void **)&m_deltaArray, sizeof(ElemType) * (m_totalModelSize), cudaHostAllocPortable));
#else
					for (int i = 0; i < m_localCacheNumber; i++)
						m_cpuAsyncBuffer[i] = new ElemType[m_totalModelSize];
#endif
				}

				float DecayCoefficient()
				{
					float f = 1.f;
					switch (m_adjustLearningRateAtBeginningType)
					{
					case AdjustLearningRateatBeginning::None:
						break;
					case AdjustLearningRateatBeginning::Linearly:
						f = min(f, max(0.f, (float)(m_adjustCoefficient + (1 - m_adjustCoefficient) / m_adjustMBNumber * m_modelSyncCount)));
						break;
					case AdjustLearningRateatBeginning::Staircase:
						f = min(f, max(0.f, (float)(m_adjustCoefficient * (m_modelSyncCount / m_adjustMBNumber + 1))));
						break;
					default:
						break;
					}
					return f;
				}
				std::vector< multiverso::MatrixWorkerTable<ElemType>*>* m_matrixArray;
				std::vector< multiverso::MatrixServerTable<ElemType>*>* m_serverArray;
				thread * m_prefetchThread;
				bool m_isInitialized;

				int m_totalClientNumber;

				bool m_isUseAsyncBuffered;
				int m_localCacheNumber;
				int * m_cacheSwapIndex;
				int m_bufferInUse;

				size_t m_modelSyncCount;

				AdjustLearningRateatBeginning m_adjustLearningRateAtBeginningType;
				double m_adjustCoefficient;
				size_t m_adjustMBNumber;

				vector<size_t> m_tableLength;
				size_t m_totalModelSize;
				vector<size_t> m_tableOffsets;
				ElemType * m_deltaArray;
				ElemType ** m_cpuAsyncBuffer;

				//GPU double buffer
				//Matrix<ElemType> ** m_gpuAsyncBuffer;
        std::vector<std::vector<Matrix<ElemType>   >> m_gpuAsyncBuffer;
				int m_tableCount;
#ifndef CPUONLY
				cudaStream_t _commStream;
#endif
			};
		}
	}
}
