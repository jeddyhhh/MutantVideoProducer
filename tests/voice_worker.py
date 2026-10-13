import importlib.util
import math
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('voice_worker',Path(__file__).parents[1]/'data/voice/worker.py')
worker=importlib.util.module_from_spec(spec);spec.loader.exec_module(worker)
class NarrationParts(unittest.TestCase):
    def test_pronunciation_preserves_original(self):
        script='Necrophagist meets Death. Necrophagists are different.'
        result=worker.speech_parts(script,{'Necrophagist':'neck roff a gist'})
        self.assertEqual(result,[('text','neck roff a gist meets Death. Necrophagists are different.')])
        self.assertIn('Necrophagist',script)
    def test_real_pauses(self):
        self.assertEqual(worker.speech_parts('First. Second [pause=0.5] third.',leading_pause=.2,trailing_pause=.3,sentence_pause=.4),[('pause',.2),('text','First.'),('pause',.4),('text','Second'),('pause',.5),('text','third.'),('pause',.3)])
    def test_invalid_pause(self):
        for script in ('a [pause=-1] b','a [pause=11] b','a [pause=oops] b','a [pause=0.4.3] b'):
            with self.assertRaises(ValueError):worker.speech_parts(script)
        for value in (math.nan,math.inf,-1,11):
            with self.assertRaises(ValueError):worker.speech_parts('hello',leading_pause=value)
    def test_limit_expansion(self):
        with self.assertRaises(ValueError):worker.speech_parts('a '*2000,{'a':'long '*99})
        with self.assertRaises(ValueError):worker.speech_parts('a [pause=10] '*100)
    def test_invalid_dictionary(self):
        for dictionary in ({1:'one'},{'word':3},{'':'nothing'}):
            with self.assertRaises(ValueError):worker.speech_parts('word',dictionary)
if __name__=='__main__':unittest.main()
