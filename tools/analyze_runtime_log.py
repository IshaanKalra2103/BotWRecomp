"""Summarize v1/v2 BotW runtime logs without assuming an abrupt ending proves a crash."""
import argparse
import json
from pathlib import Path
import re
import statistics

def analyze(text):
    result = {'duration_seconds': 0.0, 'memory': [], 'fps_samples': [], 'gpu_errors': [],
              'fatal_records': [], 'lifecycle': [], 'audio': [], 'legacy_audio_intervals': [], 'normal_exit_recorded': False}
    timestamp = None
    previous_audio_time = None
    for line in text.splitlines():
        match = re.match(r'^\[\s*([0-9.]+)\]', line)
        if match:
            timestamp = float(match[1])
            result['duration_seconds'] = max(result['duration_seconds'], timestamp)
        memory = re.search(r'footprint (\d+) (?:MB|MiB), available before jetsam (\d+)', line)
        if memory:
            result['memory'].append({'time': timestamp, 'footprint_mib': int(memory[1]), 'headroom_mib': int(memory[2])})
        fps = re.search(r'\[gpu\]\s*([0-9.]+) fps', line)
        if fps: result['fps_samples'].append({'time': timestamp, 'fps': float(fps[1])})
        if 'COMMAND BUFFER FAILED' in line: result['gpu_errors'].append({'time': timestamp, 'message': line})
        if 'FATAL' in line: result['fatal_records'].append({'time': timestamp, 'message': line})
        state = re.search(r'lifecycle: (active|inactive)', line)
        if state: result['lifecycle'].append({'time': timestamp, 'state': state[1]})
        if 'audio: level=' in line:
            item = {'time': timestamp}
            for key, value in re.findall(r'([a-z_]+)=(-?[0-9.]+)', line): item[key] = float(value)
            result['audio'].append(item)
        legacy_audio = re.search(r'audio: .*over the last (\d+) s', line)
        if legacy_audio and timestamp is not None:
            if previous_audio_time is not None and timestamp > previous_audio_time:
                elapsed = timestamp - previous_audio_time
                result['legacy_audio_intervals'].append({'start': previous_audio_time, 'end': timestamp,
                    'wall_seconds': elapsed, 'approx_produced_seconds': int(legacy_audio[1]),
                    'percent_realtime': 100 * int(legacy_audio[1]) / elapsed})
            previous_audio_time = timestamp
        if 'session: normal main loop exit' in line: result['normal_exit_recorded'] = True
    memory = result['memory']
    fps = [x['fps'] for x in result['fps_samples']]
    first_playable = next((x['time'] for x in result['fps_samples'] if x['fps'] >= 20), None)
    result['summary'] = {
        'peak_footprint_mib': max((x['footprint_mib'] for x in memory), default=None),
        'minimum_headroom_mib': min((x['headroom_mib'] for x in memory), default=None),
        'minimum_sampled_fps': min(fps, default=None),
        'median_sampled_fps': statistics.median(fps) if fps else None,
        'first_low_fps_seconds': next((x['time'] for x in result['fps_samples']
                                       if first_playable is not None and x['time'] > first_playable and x['fps'] < 5), None),
        'max_audio_gap_ms': max((x.get('max_producer_gap', 0) for x in result['audio']), default=None),
        'shortage_requests': sum(x.get('shortage_requests', 0) for x in result['audio']),
    }
    return result

def report(result):
    summary = result['summary']
    lines = ['# Анализ BotW runtime.log', '',
             f"Последняя отметка: {result['duration_seconds']:.3f} с после начала журнала.", '',
             '| Показатель | Значение |', '| --- | --- |',
             f"| Пиковая память | {summary['peak_footprint_mib']} МиБ |",
             f"| Минимальный запас до jetsam | {summary['minimum_headroom_mib']} МиБ |",
             f"| Минимальный FPS в записанных выборках | {summary['minimum_sampled_fps']} |",
             f"| Медиана записанных выборок FPS | {summary['median_sampled_fps']} |",
             f"| Записи ошибок Metal | {len(result['gpu_errors'])} |",
             f"| Аварийные записи | {len(result['fatal_records'])} |",
             f"| Переходы активности | {len(result['lifecycle'])} |", '',
             'Медиана выборок — не средний FPS всего сеанса. Загрузка, меню и фон могут давать низкие значения.', '']
    if summary['minimum_headroom_mib'] is not None and summary['minimum_headroom_mib'] < 256:
        lines += ['Обнаружен малый запас памяти (<256 МиБ). Это риск завершения iOS, но не доказательство jetsam.', '']
    if summary['first_low_fps_seconds'] is not None:
        lines += [f"После начала рендеринга (выборка ≥20 FPS) значение ниже 5 FPS впервые записано на {summary['first_low_fps_seconds']:.3f} с.", '']
    if result['normal_exit_recorded']:
        lines += ['Приложение записало штатное завершение основного цикла.', '']
    else:
        lines += ['Штатное завершение не записано. Файл мог быть экспортирован во время работы; также возможны',
                  'принудительное закрытие, jetsam или вылет. По окончанию файла одному их не различить.', '']
    if result['audio']:
        lines += [f"Максимальный записанный промежуток подачи звука: {summary['max_audio_gap_ms']} мс.",
                  f"Запросы дополнительных аудиоданных SDL: {summary['shortage_requests']:.0f}.",
                  'Это диагностические счётчики, не точное число слышимых щелчков.', '']
    else:
        lines += ['В журнале нет подробной аудиотелеметрии v2; по сообщениям уровня громкости нельзя установить причину треска.', '']
    if result['legacy_audio_intervals']:
        slowest = min(result['legacy_audio_intervals'], key=lambda item: item['percent_realtime'])
        lines += [f"По старому счётчику семплов на участке {slowest['start']:.3f}–{slowest['end']:.3f} с "
                  f"подготовка примерно {slowest['approx_produced_seconds']} с аудио заняла {slowest['wall_seconds']:.3f} с "
                  f"реального времени ({slowest['percent_realtime']:.1f}% необходимого темпа).",
                  'Это оценка темпа генерации по двум сообщениям, а не измерение слышимой тональности или длительности щелчков.', '']
    if result['gpu_errors'] or result['fatal_records']:
        lines += ['## Записи ошибок', '']
        lines += ['    ' + item['message'] for item in result['gpu_errors'] + result['fatal_records']]
    return '\n'.join(lines) + '\n'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.log.read_text(encoding='utf-8', errors='replace'))
    output = json.dumps(result, ensure_ascii=False, indent=2) if args.json else report(result)
    if args.output: args.output.write_text(output, encoding='utf-8')
    else: print(output)

if __name__ == '__main__': main()
