"""Loopback-only Messages/OpenAI mock for desktop integration tests; no real keys."""
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps({'object': 'list', 'data': [
            {'id': 'deepseek-flash', 'object': 'model', 'owned_by': 'local-mock'}
        ]}).encode())

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        print('mock request', self.path, 'stream=', bool(request.get('stream')), flush=True)
        text = '本地 Mock 验证完成。没有调用真实模型服务。'
        if 'chat/completions' in self.path:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream' if request.get('stream') else 'application/json')
            self.end_headers()
            if not request.get('stream'):
                self.wfile.write(json.dumps({'id':'mock','object':'chat.completion','model':'mock-chat',
                    'choices':[{'index':0,'message':{'role':'assistant','content':text},'finish_reason':'stop'}],
                    'usage':{'prompt_tokens':10,'completion_tokens':20,'total_tokens':30}}).encode())
                return
            try:
                for chunk in [text[:10],text[10:20],text[20:]]:
                    time.sleep(3)
                    row={'id':'mock','object':'chat.completion.chunk','model':'mock-chat',
                        'choices':[{'index':0,'delta':{'role':'assistant','content':chunk},'finish_reason':None}]}
                    self.wfile.write(('data: '+json.dumps(row)+'\n\n').encode());self.wfile.flush()
                row['choices']=[{'index':0,'delta':{},'finish_reason':'stop'}]
                self.wfile.write(('data: '+json.dumps(row)+'\n\ndata: [DONE]\n\n').encode());self.wfile.flush()
            except (BrokenPipeError,ConnectionResetError):
                pass
            return
        if not request.get('stream'):
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({'id': 'mock', 'type': 'message', 'role': 'assistant',
                'model': 'deepseek-flash', 'content': [{'type': 'text', 'text': text}],
                'stop_reason': 'end_turn', 'usage': {'input_tokens': 10, 'output_tokens': 20}}).encode())
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        def event(kind, data):
            self.wfile.write(('event: '+kind+'\ndata: '+json.dumps(data, ensure_ascii=False)+'\n\n').encode())
            self.wfile.flush()
        try:
            event('message_start', {'type': 'message_start', 'message': {'id': 'mock', 'type': 'message',
                'role': 'assistant', 'model': 'deepseek-flash', 'content': [], 'stop_reason': None,
                'usage': {'input_tokens': 10, 'output_tokens': 0}}})
            event('content_block_start', {'type': 'content_block_start', 'index': 0,
                'content_block': {'type': 'text', 'text': ''}})
            for chunk in [text[:10], text[10:20], text[20:]]:
                time.sleep(3)
                event('content_block_delta', {'type': 'content_block_delta', 'index': 0,
                    'delta': {'type': 'text_delta', 'text': chunk}})
            event('content_block_stop', {'type': 'content_block_stop', 'index': 0})
            event('message_delta', {'type': 'message_delta', 'delta': {'stop_reason': 'end_turn',
                'stop_sequence': None}, 'usage': {'output_tokens': 20}})
            event('message_stop', {'type': 'message_stop'})
        except (BrokenPipeError, ConnectionResetError):
            pass


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=18765)
    port = parser.parse_args().port
    print(f'Mock listening on http://127.0.0.1:{port}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
