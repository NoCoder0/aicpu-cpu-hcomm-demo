"""核对逐轮成功日志并汇总 msprof 导出数据；只有成功且样本数一致才接受结果。"""
import argparse
import csv
from decimal import Decimal
import hashlib
import json
import math
from pathlib import Path
import statistics


def summary(values):
    values = sorted(values)
    assert values
    return {'count': len(values), 'mean_us': statistics.mean(values),
            **{f'p{p}_us': values[math.ceil(len(values) * p / 100) - 1] for p in (50, 95, 99)},
            'min_us': values[0], 'max_us': values[-1]}


def analyze(log, profile=None, kernel='StandardReadKernel', host_log=None):
    text = log.read_text(encoding='utf-8')
    assert 'PASS:' in text and 'FAIL' not in text and 'Mismatch' not in text, 'application did not pass'
    samples = [dict(item.split('=', 1) for item in line.split()[1:])
               for line in text.splitlines() if line.startswith('SAMPLE ')]
    assert samples and all(int(s['generation']) == i + 1 and s['verified'] == '1'
                           for i, s in enumerate(samples)), 'missing/unverified generations'
    assert all(s['warmup'] in ('0', '1') and s['blocks'] == '1600' and
               s['block_bytes'] == '656' and s['payload_bytes'] == '1049600' for s in samples)
    warmup = sum(s['warmup'] == '1' for s in samples)
    assert all(s['warmup'] == '1' for s in samples[:warmup]), 'warmup must be a prefix'
    metrics = [k for k in samples[0] if k.endswith('_us')]
    result = {'kernel': kernel, 'warmup': warmup, 'measured_rounds': len(samples) - warmup,
              'application': {key: summary([float(s[key]) for s in samples[warmup:]]) for key in metrics},
              'sha256': {str(log): hashlib.sha256(log.read_bytes()).hexdigest()}}
    if kernel == 'AggregateKernel':
        assert host_log is not None, 'aggregate analysis requires --host-log'
        host_text = host_log.read_text(encoding='utf-8')
        assert 'PASS:' in host_text and 'FAIL' not in host_text, 'Host application did not pass'
        host_samples = [dict(item.split('=', 1) for item in line.split()[1:])
                        for line in host_text.splitlines() if line.startswith('HOST_SAMPLE ')]
        assert len(host_samples) == len(samples) and all(
            h['generation'] == s['generation'] for h, s in zip(host_samples, samples)), 'Host generation mismatch'
        assert all(abs(sum(float(s[k]) for k in ('request_us', 'wait_host_us', 'scatter_us')) -
                       float(s['kernel_e2e_us'])) <= 0.004 for s in samples), 'aggregate phase timing mismatch'
        result['host_application'] = {
            k: summary([float(s[k]) for s in host_samples[warmup:]])
            for k in host_samples[0] if k.endswith('_us')}
        result['sha256'][str(host_log)] = hashlib.sha256(host_log.read_bytes()).hexdigest()
    if profile:
        def read_one(pattern):
            paths = list(profile.glob(pattern))
            assert len(paths) == 1, f'expected one {pattern}, got {len(paths)}'
            path = paths[0]
            result['sha256'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
            with path.open(encoding='utf-8-sig', newline='') as f:
                return list(csv.DictReader(f))

        tasks = read_one('task_time*.csv')
        kernels = sorted((r for r in tasks if r['kernel_name'] == kernel), key=lambda r: float(r['task_start(us)']))
        assert len(kernels) == len(samples), 'profiling kernel count differs from verified application count'
        result['profiling'] = {'kernel_task': summary([float(r['task_time(us)']) for r in kernels[warmup:]])}
        aicpu = [r for r in read_one('aicpu*.csv') if r['Node'] == kernel]
        aicpu.sort(key=lambda r: float(r['Timestamp(us)']))
        lanes = 6 if kernel == 'AggregateKernel' else 1
        assert len(aicpu) == lanes * len(samples), 'AICPU lane sample count mismatch'
        groups = [aicpu[i:i + lanes] for i in range(0, len(aicpu), lanes)]
        for task, group in zip(kernels, groups):
            assert all(Decimal(task['task_start(us)'].strip()) <= Decimal(row['Timestamp(us)'].strip()) <=
                       Decimal(task['task_stop(us)'].strip()) for row in group), 'AICPU lane/launch mismatch'
        if lanes == 1:
            result['profiling']['aicpu_task'] = summary([float(r['Task_time(us)']) for r in aicpu[warmup:]])
            result['profiling']['aicpu_total'] = summary([float(r['Total_time(us)']) for r in aicpu[warmup:]])
        else:
            result['profiling']['lanes_per_launch'] = lanes
            for column, name in [('Task_time(us)', 'aicpu_slowest_lane_task'),
                                 ('Total_time(us)', 'aicpu_slowest_lane_total')]:
                result['profiling'][name] = summary([max(float(r[column]) for r in g) for g in groups[warmup:]])

        # 仅基础版：每轮通信线程的 Record 前应有1601次 doorbell（1600 READ + Drain READ）。
        # 这统计的是 TS 任务，不是 RoCE 线上报文，也不是纯网络时间。
        if kernel == 'StandardReadKernel':
            records = [r for r in tasks if r['kernel_type'] == 'NOTIFY_RECORD_SQE']
            assert len(records) == len(samples), 'completion record count mismatch'
            stream_ids = {r['stream_id'] for r in records}
            assert len(stream_ids) == 1, 'ambiguous communication stream'
            comm = sorted((r for r in tasks if r['stream_id'] in stream_ids),
                          key=lambda r: float(r['task_start(us)']))
            begin, writes, waits, intervals, ends = None, 0, 0, [], []
            for row in comm:
                kind = row['kernel_type']
                if kind == 'WRITE_VALUE_SQE':
                    if begin is None:
                        begin = Decimal(row['task_start(us)'].strip())
                    writes += 1
                elif kind == 'NOTIFY_WAIT_SQE':
                    waits += 1
                elif kind == 'NOTIFY_RECORD_SQE':
                    assert writes == 1601 and waits == 1 and begin is not None, 'incomplete READ/Drain task group'
                    end = Decimal(row['task_stop(us)'].strip())
                    intervals.append(float(end - begin))
                    ends.append(end)
                    begin, writes, waits = None, 0, 0
            assert writes == waits == 0 and len(intervals) == len(samples)
            result['profiling']['communication_task_span'] = summary(intervals[warmup:])
            result['profiling']['kernel_start_to_completion_record'] = summary(
                [float(end - Decimal(row['task_start(us)'].strip())) for end, row in zip(ends, kernels)][warmup:])
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--kernel', default='StandardReadKernel')
    parser.add_argument('--host-log', type=Path, help='aggregate Host log with matching generations')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = json.dumps(analyze(args.log, args.profile, args.kernel, args.host_log), indent=2, ensure_ascii=False) + '\n'
    if args.output:
        args.output.write_text(output, encoding='utf-8')
    print(output)
