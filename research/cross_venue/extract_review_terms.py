"""Extract saved, hashed official contract PDFs for a human reviewer.

Optional research dependency: pypdf. This does not interpret or approve rules.
Never fetches documents; extraction uses only the saved successful responses.
"""
import argparse
import hashlib
import json
from pathlib import Path


def extract(directory):
    import pypdf
    results=[]
    for entry in json.loads((directory/'source_requests.json').read_text()):
        path=directory/entry['file']
        raw=path.read_bytes()
        if hashlib.sha256(raw).hexdigest()!=entry['sha256']:raise ValueError('document source hash mismatch')
        if entry['http_status']!=200 or not raw.startswith(b'%PDF'):continue
        reader=pypdf.PdfReader(path)
        text='\n\n'.join(page.extract_text() for page in reader.pages)
        target=entry['file']+'.txt';(directory/target).write_text(text)
        results.append({'source':entry['source'],'file':target,'source_sha256':entry['sha256'],
            'text_sha256':hashlib.sha256(text.encode()).hexdigest(),'pages':len(reader.pages),'extractor':'pypdf '+pypdf.__version__})
    (directory/'pdf_extraction_manifest.json').write_text(json.dumps(results,indent=2)+'\n')
    return results


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--directory',required=True,type=Path)
    print(json.dumps(extract(parser.parse_args().directory),indent=2))
