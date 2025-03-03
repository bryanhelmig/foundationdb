/*
 * LowLatency.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/IKnobCollection.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct LowLatencyWorkload : TestWorkload {
	double testDuration;
	double maxGRVLatency;
	double maxCommitLatency;
	double checkDelay;
	PerfIntCounter operations, retries;
	bool testWrites;
	Key testKey;
	bool ok;

	LowLatencyWorkload(WorkloadContext const& wcx)
	  : TestWorkload(wcx), operations("Operations"), retries("Retries"), ok(true) {
		testDuration = getOption(options, LiteralStringRef("testDuration"), 600.0);
		maxGRVLatency = getOption(options, LiteralStringRef("maxGRVLatency"), 20.0);
		maxCommitLatency = getOption(options, LiteralStringRef("maxCommitLatency"), 30.0);
		checkDelay = getOption(options, LiteralStringRef("checkDelay"), 1.0);
		testWrites = getOption(options, LiteralStringRef("testWrites"), true);
		testKey = getOption(options, LiteralStringRef("testKey"), LiteralStringRef("testKey"));
	}

	std::string description() const override { return "LowLatency"; }

	Future<Void> setup(Database const& cx) override {
		if (g_network->isSimulated()) {
			IKnobCollection::getMutableGlobalKnobCollection().setKnob("min_delay_cc_worst_fit_candidacy_seconds",
			                                                          KnobValueRef::create(double{ 5.0 }));
			IKnobCollection::getMutableGlobalKnobCollection().setKnob("max_delay_cc_worst_fit_candidacy_seconds",
			                                                          KnobValueRef::create(double{ 10.0 }));
		}
		return Void();
	}

	Future<Void> start(Database const& cx) override {
		if (clientId == 0)
			return _start(cx, this);
		return Void();
	}

	ACTOR static Future<Void> _start(Database cx, LowLatencyWorkload* self) {
		state double testStart = now();
		try {
			loop {
				wait(delay(self->checkDelay));
				state Transaction tr(cx);
				state double operationStart = now();
				state bool doCommit = self->testWrites && deterministicRandom()->coinflip();
				state double maxLatency = doCommit ? self->maxCommitLatency : self->maxGRVLatency;
				++self->operations;
				loop {
					try {
						TraceEvent("StartLowLatencyTransaction").log();
						tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
						tr.setOption(FDBTransactionOptions::LOCK_AWARE);
						if (doCommit) {
							tr.set(self->testKey, LiteralStringRef(""));
							wait(tr.commit());
						} else {
							wait(success(tr.getReadVersion()));
						}
						break;
					} catch (Error& e) {
						TraceEvent("LowLatencyTransactionFailed").error(e, true);
						wait(tr.onError(e));
						++self->retries;
					}
				}
				if (now() - operationStart > maxLatency) {
					TraceEvent(SevError, "LatencyTooLarge")
					    .detail("MaxLatency", maxLatency)
					    .detail("ObservedLatency", now() - operationStart)
					    .detail("IsCommit", doCommit);
					self->ok = false;
				}
				if (now() - testStart > self->testDuration)
					break;
			}
			return Void();
		} catch (Error& e) {
			TraceEvent(SevError, "LowLatencyError").error(e, true);
			throw;
		}
	}

	Future<bool> check(Database const& cx) override { return ok; }

	void getMetrics(vector<PerfMetric>& m) override {
		double duration = testDuration;
		m.emplace_back("Operations/sec", operations.getValue() / duration, Averaged::False);
		m.push_back(operations.getMetric());
		m.push_back(retries.getMetric());
	}
};

WorkloadFactory<LowLatencyWorkload> LowLatencyWorkloadFactory("LowLatency");
