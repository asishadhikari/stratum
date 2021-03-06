#
# Copyright 2019-present Open Networking Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# This Dockerfile expects the stratum root as its scope, hence you should build
# from root e.g.:
# docker build -t <some tag> -f stratum/p4c_backends/fpm/Dockerfile .

# We use a 2-stage build.
FROM stratumproject/build:build as builder

# Build FPM backend
COPY . /tmp/stratum
WORKDIR /tmp/stratum

RUN bazel build //stratum/p4c_backends/fpm:p4c-fpm

# Put aside binary and other runtime dependencies.
RUN mkdir -p /output/usr/local/bin
RUN install bazel-bin/stratum/p4c_backends/fpm/p4c-fpm /output/usr/local/bin/

RUN mkdir -p /output/usr/share/p4c
RUN cp -r $(bazel info output_base)/external/com_github_p4lang_p4c/p4include /output/usr/share/p4c/

# Final stage, runtime.
FROM bitnami/minideb:stretch as runtime

LABEL maintainer="Stratum dev <stratum-dev@lists.stratumproject.org>"
LABEL description="Docker-based distribution of the Stratum p4c FPM backend"

ENV RUNTIME_DEPS libgmp10 libgmpxx4ldbl gcc
RUN install_packages ${RUNTIME_DEPS}

COPY --from=builder /output /

ENTRYPOINT ["p4c-fpm"]
CMD ["-help"]
