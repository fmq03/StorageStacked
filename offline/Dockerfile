# Build from a prepared local snapshot; every RUN executes without networking.
ARG BASE_IMAGE=ubuntu:20.04
FROM ${BASE_IMAGE}
ARG WORKSPACE_ROOT=/home/hy258/hy/zhongxing
ENV STORAGE_WORKSPACE=${WORKSPACE_ROOT} SS_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 \
    LANG=C.UTF-8 LC_ALL=C.UTF-8
ADD --chown=0:0 workspace.tar ${WORKSPACE_ROOT}/
COPY entrypoint.py /opt/storagestacked/entrypoint.py
COPY docker-compat-entrypoint.py /opt/storagestacked/docker-compat-entrypoint.py
COPY fix-native-cache.sh /opt/storagestacked/fix-native-cache.sh
COPY prepare.sh /opt/storagestacked/prepare.sh
COPY system-tools/ /opt/storagestacked/system-tools/
RUN bash /opt/storagestacked/prepare.sh
WORKDIR ${WORKSPACE_ROOT}
ENTRYPOINT ["/usr/local/bin/python3", "/opt/storagestacked/docker-compat-entrypoint.py"]
CMD ["all"]
