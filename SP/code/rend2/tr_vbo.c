/*
===========================================================================
Copyright (C) 2007-2009 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_vbo.c
#include "tr_local.h"


void R_VaoPackTangent(int16_t *out, vec4_t v)
{
	out[0] = v[0] * 32767.0f + (v[0] > 0.0f ? 0.5f : -0.5f);
	out[1] = v[1] * 32767.0f + (v[1] > 0.0f ? 0.5f : -0.5f);
	out[2] = v[2] * 32767.0f + (v[2] > 0.0f ? 0.5f : -0.5f);
	out[3] = v[3] * 32767.0f + (v[3] > 0.0f ? 0.5f : -0.5f);
}

void R_VaoPackNormal(int16_t *out, vec3_t v)
{
	out[0] = v[0] * 32767.0f + (v[0] > 0.0f ? 0.5f : -0.5f);
	out[1] = v[1] * 32767.0f + (v[1] > 0.0f ? 0.5f : -0.5f);
	out[2] = v[2] * 32767.0f + (v[2] > 0.0f ? 0.5f : -0.5f);
	out[3] = 0;
}

void R_VaoPackColor(uint16_t *out, vec4_t c)
{
	out[0] = c[0] * 65535.0f + 0.5f;
	out[1] = c[1] * 65535.0f + 0.5f;
	out[2] = c[2] * 65535.0f + 0.5f;
	out[3] = c[3] * 65535.0f + 0.5f;
}

void R_VaoUnpackTangent(vec4_t v, int16_t *pack)
{
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
	v[3] = pack[3] / 32767.0f;
}

void R_VaoUnpackNormal(vec3_t v, int16_t *pack)
{
	v[0] = pack[0] / 32767.0f;
	v[1] = pack[1] / 32767.0f;
	v[2] = pack[2] / 32767.0f;
}

void Vao_SetVertexPointers(vao_t *vao)
{
	int attribIndex;

	// set vertex pointers
	for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
	{
		uint32_t attribBit = 1 << attribIndex;
		vaoAttrib_t *vAtb = &vao->attribs[attribIndex];

		if (vAtb->enabled)
		{
			qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));
			if (glRefConfig.vertexArrayObject || !(glState.vertexAttribsEnabled & attribBit))
				qglEnableVertexAttribArray(attribIndex);

			if (!glRefConfig.vertexArrayObject || vao == tess.vao)
				glState.vertexAttribsEnabled |= attribBit;
		}
		else
		{
			// don't disable vertex attribs when using vertex array objects
			// Vao_SetVertexPointers is only called during init when using VAOs, and vertex attribs start disabled anyway
			if (!glRefConfig.vertexArrayObject && (glState.vertexAttribsEnabled & attribBit))
				qglDisableVertexAttribArray(attribIndex);

			if (!glRefConfig.vertexArrayObject || vao == tess.vao)
				glState.vertexAttribsEnabled &= ~attribBit;
		}
	}
}

/*
============
R_CreateVao
============
*/
vao_t *R_CreateVao(const char *name, byte *vertexes, int vertexesSize, byte *indexes, int indexesSize, vaoUsage_t usage)
{
	vao_t          *vao;
	int				glUsage;

	switch (usage)
	{
		case VAO_USAGE_STATIC:
			glUsage = GL_STATIC_DRAW;
			break;

		case VAO_USAGE_DYNAMIC:
			glUsage = GL_DYNAMIC_DRAW;
			break;

		default:
			ri.Error(ERR_FATAL, "bad vaoUsage_t given: %i", usage);
			return NULL;
	}

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "R_CreateVao: \"%s\" is too long", name);
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao: MAX_VAOS hit");
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = ri.Hunk_Alloc(sizeof(*vao), h_low);
	tr.numVaos++;

	memset(vao, 0, sizeof(*vao));

	Q_strncpyz(vao->name, name, sizeof(vao->name));


	if (glRefConfig.vertexArrayObject)
	{
		qglGenVertexArrays(1, &vao->vao);
		qglBindVertexArray(vao->vao);
	}


	vao->vertexesSize = vertexesSize;

	qglGenBuffers(1, &vao->vertexesVBO);

	qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vertexesSize, vertexes, glUsage);


	vao->indexesSize = indexesSize;

	qglGenBuffers(1, &vao->indexesIBO);

	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, indexesSize, indexes, glUsage);


	glState.currentVao = vao;

	GL_CheckErrors();

	return vao;
}

/*
============
R_CreateVao2
============
*/
vao_t *R_CreateVao2(const char *name, int numVertexes, srfVert_t *verts, int numIndexes, glIndex_t *indexes)
{
	vao_t          *vao;
	int             i;

	byte           *data;
	int             dataSize;
	int             dataOfs;

	int				glUsage = GL_STATIC_DRAW;

	if(!numVertexes || !numIndexes)
		return NULL;

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "R_CreateVao2: \"%s\" is too long", name);
	}

	if ( tr.numVaos == MAX_VAOS ) {
		ri.Error( ERR_DROP, "R_CreateVao2: MAX_VAOS hit");
	}

	R_IssuePendingRenderCommands();

	vao = tr.vaos[tr.numVaos] = ri.Hunk_Alloc(sizeof(*vao), h_low);
	tr.numVaos++;

	memset(vao, 0, sizeof(*vao));

	Q_strncpyz(vao->name, name, sizeof(vao->name));

	// since these vertex attributes are never altered, interleave them
	vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;

	vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;

	vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;

	vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;

	vao->attribs[ATTR_INDEX_POSITION      ].offset = 0;        dataSize  = sizeof(verts[0].xyz);
	vao->attribs[ATTR_INDEX_NORMAL        ].offset = dataSize; dataSize += sizeof(verts[0].normal);
	vao->attribs[ATTR_INDEX_TANGENT       ].offset = dataSize; dataSize += sizeof(verts[0].tangent);
	vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = dataSize; dataSize += sizeof(verts[0].st);
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = dataSize; dataSize += sizeof(verts[0].lightmap);
	vao->attribs[ATTR_INDEX_COLOR         ].offset = dataSize; dataSize += sizeof(verts[0].color);
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = dataSize; dataSize += sizeof(verts[0].lightdir);

	vao->attribs[ATTR_INDEX_POSITION      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_NORMAL        ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TANGENT       ].stride = dataSize;
	vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = dataSize;
	vao->attribs[ATTR_INDEX_COLOR         ].stride = dataSize;
	vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = dataSize;


	if (glRefConfig.vertexArrayObject)
	{
		qglGenVertexArrays(1, &vao->vao);
		qglBindVertexArray(vao->vao);
	}


	// create VBO
	dataSize *= numVertexes;
	data = ri.Hunk_AllocateTempMemory(dataSize);
	dataOfs = 0;

	for (i = 0; i < numVertexes; i++)
	{
		// xyz
		memcpy(data + dataOfs, &verts[i].xyz, sizeof(verts[i].xyz));
		dataOfs += sizeof(verts[i].xyz);

		// normal
		memcpy(data + dataOfs, &verts[i].normal, sizeof(verts[i].normal));
		dataOfs += sizeof(verts[i].normal);

		// tangent
		memcpy(data + dataOfs, &verts[i].tangent, sizeof(verts[i].tangent));
		dataOfs += sizeof(verts[i].tangent);

		// texcoords
		memcpy(data + dataOfs, &verts[i].st, sizeof(verts[i].st));
		dataOfs += sizeof(verts[i].st);

		// lightmap texcoords
		memcpy(data + dataOfs, &verts[i].lightmap, sizeof(verts[i].lightmap));
		dataOfs += sizeof(verts[i].lightmap);

		// colors
		memcpy(data + dataOfs, &verts[i].color, sizeof(verts[i].color));
		dataOfs += sizeof(verts[i].color);

		// light directions
		memcpy(data + dataOfs, &verts[i].lightdir, sizeof(verts[i].lightdir));
		dataOfs += sizeof(verts[i].lightdir);
	}

	vao->vertexesSize = dataSize;

	qglGenBuffers(1, &vao->vertexesVBO);

	qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vao->vertexesSize, data, glUsage);


	// create IBO
	vao->indexesSize = numIndexes * sizeof(glIndex_t);

	qglGenBuffers(1, &vao->indexesIBO);

	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vao->indexesSize, indexes, glUsage);


	Vao_SetVertexPointers(vao);


	glState.currentVao = vao;

	GL_CheckErrors();

	ri.Hunk_FreeTempMemory(data);

	return vao;
}


/*
============
R_BindVao
============
*/
void R_BindVao(vao_t * vao)
{
	if(!vao)
	{
		//R_BindNullVao();
		ri.Error(ERR_DROP, "R_BindVao: NULL vao");
		return;
	}

	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get a call to va() every frame!
		GLimp_LogComment(va("--- R_BindVao( %s ) ---\n", vao->name));
	}

	if(glState.currentVao != vao)
	{
		glState.currentVao = vao;

		glState.vertexAttribsInterpolation = 0;
		glState.vertexAnimation = qfalse;
		backEnd.pc.c_vaoBinds++;

		if (glRefConfig.vertexArrayObject)
		{
			qglBindVertexArray(vao->vao);

			// Intel Graphics doesn't save GL_ELEMENT_ARRAY_BUFFER binding with VAO binding.
			if (glRefConfig.intelGraphics || vao == tess.vao)
				qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);

			// tess VAO always has buffers bound
			if (vao == tess.vao)
				qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
		}
		else
		{
			qglBindBuffer(GL_ARRAY_BUFFER, vao->vertexesVBO);
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vao->indexesIBO);

			// tess VAO doesn't have vertex pointers set until data is uploaded
			if (vao != tess.vao)
				Vao_SetVertexPointers(vao);
		}
	}
}

/*
============
R_BindNullVao
============
*/
void R_BindNullVao(void)
{
	GLimp_LogComment("--- R_BindNullVao ---\n");

	if(glState.currentVao)
	{
		if (glRefConfig.vertexArrayObject)
		{
			qglBindVertexArray(0);

			// why you no save GL_ELEMENT_ARRAY_BUFFER binding, Intel?
			if (1) qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		}
		else
		{
			qglBindBuffer(GL_ARRAY_BUFFER, 0);
			qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		}
		glState.currentVao = NULL;
	}

	GL_CheckErrors();
}


/*
============
R_InitVaos
============
*/
void R_InitVaos(void)
{
	int             vertexesSize, indexesSize;
	int             offset;

	ri.Printf(PRINT_ALL, "------- R_InitVaos -------\n");

	tr.numVaos = 0;

	vertexesSize  = sizeof(tess.xyz[0]);
	vertexesSize += sizeof(tess.normal[0]);
	vertexesSize += sizeof(tess.tangent[0]);
	vertexesSize += sizeof(tess.color[0]);
	vertexesSize += sizeof(tess.texCoords[0]);
	vertexesSize += sizeof(tess.lightCoords[0]);
	vertexesSize += sizeof(tess.lightdir[0]);
	vertexesSize += sizeof(tess.xyzPrev[0]);
	vertexesSize *= SHADER_MAX_VERTEXES;

	indexesSize = sizeof(tess.indexes[0]) * SHADER_MAX_INDEXES;

	tess.vao = R_CreateVao("tessVertexArray_VAO", NULL, vertexesSize, NULL, indexesSize, VAO_USAGE_DYNAMIC);

	offset = 0;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].enabled = 1;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].count = 3;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].count = 4;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].count = 2;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].count = 2;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].count = 4;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].count = 3;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].type = GL_FLOAT;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].type = GL_UNSIGNED_SHORT;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type = GL_SHORT;
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].type = GL_FLOAT;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].normalized = GL_FALSE;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].normalized = GL_FALSE;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].offset = offset; offset += sizeof(tess.xyz[0])         * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].offset = offset; offset += sizeof(tess.normal[0])      * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].offset = offset; offset += sizeof(tess.tangent[0])     * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].offset = offset; offset += sizeof(tess.texCoords[0])   * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].offset = offset; offset += sizeof(tess.lightCoords[0]) * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_COLOR         ].offset = offset; offset += sizeof(tess.color[0])       * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = offset; offset += sizeof(tess.lightdir[0])      * SHADER_MAX_VERTEXES;
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].offset = offset;

	tess.vao->attribs[ATTR_INDEX_POSITION      ].stride = sizeof(tess.xyz[0]);
	tess.vao->attribs[ATTR_INDEX_NORMAL        ].stride = sizeof(tess.normal[0]);
	tess.vao->attribs[ATTR_INDEX_TANGENT       ].stride = sizeof(tess.tangent[0]);
	tess.vao->attribs[ATTR_INDEX_TEXCOORD      ].stride = sizeof(tess.texCoords[0]);
	tess.vao->attribs[ATTR_INDEX_LIGHTCOORD    ].stride = sizeof(tess.lightCoords[0]);
	tess.vao->attribs[ATTR_INDEX_COLOR         ].stride = sizeof(tess.color[0]);
	tess.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = sizeof(tess.lightdir[0]);
	tess.vao->attribs[ATTR_INDEX_POSITION2     ].stride = sizeof(tess.xyzPrev[0]);

	tess.attribPointers[ATTR_INDEX_POSITION]       = tess.xyz;
	tess.attribPointers[ATTR_INDEX_NORMAL]         = tess.normal;
	tess.attribPointers[ATTR_INDEX_TANGENT]        = tess.tangent;
	tess.attribPointers[ATTR_INDEX_TEXCOORD]       = tess.texCoords;
	tess.attribPointers[ATTR_INDEX_LIGHTCOORD]     = tess.lightCoords;
	tess.attribPointers[ATTR_INDEX_COLOR]          = tess.color;
	tess.attribPointers[ATTR_INDEX_LIGHTDIRECTION] = tess.lightdir;
	tess.attribPointers[ATTR_INDEX_POSITION2]      = tess.xyzPrev;

	Vao_SetVertexPointers(tess.vao);

	R_BindNullVao();

	VaoCache_Init();

	GL_CheckErrors();
}

/*
============
R_ShutdownVaos
============
*/
void R_ShutdownVaos(void)
{
	int             i;
	vao_t          *vao;

	ri.Printf(PRINT_ALL, "------- R_ShutdownVaos -------\n");

	R_BindNullVao();

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		if(vao->vao)
			qglDeleteVertexArrays(1, &vao->vao);

		if(vao->vertexesVBO)
		{
			qglDeleteBuffers(1, &vao->vertexesVBO);
		}

		if(vao->indexesIBO)
		{
			qglDeleteBuffers(1, &vao->indexesIBO);
		}
	}

	tr.numVaos = 0;
}

/*
============
R_VaoList_f
============
*/
void R_VaoList_f(void)
{
	int             i;
	vao_t          *vao;
	int             vertexesSize = 0;
	int             indexesSize = 0;

	ri.Printf(PRINT_ALL, " size          name\n");
	ri.Printf(PRINT_ALL, "----------------------------------------------------------\n");

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf(PRINT_ALL, "%d.%02d MB %s\n", vao->vertexesSize / (1024 * 1024),
				  (vao->vertexesSize % (1024 * 1024)) * 100 / (1024 * 1024), vao->name);

		vertexesSize += vao->vertexesSize;
	}

	for(i = 0; i < tr.numVaos; i++)
	{
		vao = tr.vaos[i];

		ri.Printf(PRINT_ALL, "%d.%02d MB %s\n", vao->indexesSize / (1024 * 1024),
				  (vao->indexesSize % (1024 * 1024)) * 100 / (1024 * 1024), vao->name);

		indexesSize += vao->indexesSize;
	}

	ri.Printf(PRINT_ALL, " %i total VAOs\n", tr.numVaos);
	ri.Printf(PRINT_ALL, " %d.%02d MB total vertices memory\n", vertexesSize / (1024 * 1024),
			  (vertexesSize % (1024 * 1024)) * 100 / (1024 * 1024));
	ri.Printf(PRINT_ALL, " %d.%02d MB total triangle indices memory\n", indexesSize / (1024 * 1024),
			  (indexesSize % (1024 * 1024)) * 100 / (1024 * 1024));
}


/*
==============
RB_UpdateTessVao

Adapted from Tess_UpdateVBOs from xreal

Update the default VAO to replace the client side vertex arrays
==============
*/
void RB_UpdateTessVao(unsigned int attribBits)
{
	GLimp_LogComment("--- RB_UpdateTessVao ---\n");

	backEnd.pc.c_dynamicVaoDraws++;

	// update the default VAO
	if(tess.numVertexes > 0 && tess.numVertexes <= SHADER_MAX_VERTEXES && tess.numIndexes > 0 && tess.numIndexes <= SHADER_MAX_INDEXES)
	{
		int attribIndex;
		int attribUpload;

		R_BindVao(tess.vao);

		// orphan old vertex buffer so we don't stall on it
		qglBufferData(GL_ARRAY_BUFFER, tess.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW);

		// if nothing to set, set everything
		if(!(attribBits & ATTR_BITS))
			attribBits = ATTR_BITS;

		attribUpload = attribBits;

		for (attribIndex = 0; attribIndex < ATTR_INDEX_COUNT; attribIndex++)
		{
			uint32_t attribBit = 1 << attribIndex;
			vaoAttrib_t *vAtb = &tess.vao->attribs[attribIndex];

			if (attribUpload & attribBit)
			{
				// note: tess has a VBO where stride == size
				qglBufferSubData(GL_ARRAY_BUFFER, vAtb->offset, tess.numVertexes * vAtb->stride, tess.attribPointers[attribIndex]);
			}

			if (attribBits & attribBit)
			{
				if (!glRefConfig.vertexArrayObject)
					qglVertexAttribPointer(attribIndex, vAtb->count, vAtb->type, vAtb->normalized, vAtb->stride, BUFFER_OFFSET(vAtb->offset));

				if (!(glState.vertexAttribsEnabled & attribBit))
				{
					qglEnableVertexAttribArray(attribIndex);
					glState.vertexAttribsEnabled |= attribBit;
				}
			}
			else
			{
				if ((glState.vertexAttribsEnabled & attribBit))
				{
					qglDisableVertexAttribArray(attribIndex);
					glState.vertexAttribsEnabled &= ~attribBit;
				}
			}
		}

		// orphan old index buffer so we don't stall on it
		qglBufferData(GL_ELEMENT_ARRAY_BUFFER, tess.vao->indexesSize, NULL, GL_DYNAMIC_DRAW);

		qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, tess.numIndexes * sizeof(tess.indexes[0]), tess.indexes);
	}
}

// FIXME: This sets a limit of 65536 verts/262144 indexes per static surface
// This is higher than the old vq3 limits but is worth noting
#define VAOCACHE_QUEUE_MAX_SURFACES (1 << 10)
#define VAOCACHE_QUEUE_MAX_VERTEXES (1 << 16)
#define VAOCACHE_QUEUE_MAX_INDEXES (VAOCACHE_QUEUE_MAX_VERTEXES * 4)

typedef struct queuedSurface_s
{
	srfVert_t *vertexes;
	int numVerts;
	glIndex_t *indexes;
	int numIndexes;
}
queuedSurface_t;

static struct
{
	queuedSurface_t surfaces[VAOCACHE_QUEUE_MAX_SURFACES];
	int numSurfaces;

	srfVert_t vertexes[VAOCACHE_QUEUE_MAX_VERTEXES];
	int vertexCommitSize;

	glIndex_t indexes[VAOCACHE_QUEUE_MAX_INDEXES];
	int indexCommitSize;
}
vcq;

// srfVert_t is 60 bytes
// assuming each vert is referenced 4 times, need 16 bytes (4 glIndex_t) per vert
// -> need about 4/15ths the space for indexes as vertexes
#if GL_INDEX_TYPE == GL_UNSIGNED_SHORT
#define VAOCACHE_VERTEX_BUFFER_SIZE (sizeof(srfVert_t) * USHRT_MAX)
#define VAOCACHE_INDEX_BUFFER_SIZE (sizeof(glIndex_t) * USHRT_MAX * 4)
#else // GL_UNSIGNED_INT
// codmod: sized for the bevelled world (~0.5M verts / ~1.5M indexes, most of it visible): the old 16/5 MB overflowed every frame
// and recycled (re-uploaded) the cache per pass. ~60 MB + 16 MB of GPU memory.
#define VAOCACHE_VERTEX_BUFFER_SIZE (sizeof(srfVert_t) * (1 << 20))
#define VAOCACHE_INDEX_BUFFER_SIZE (sizeof(glIndex_t) * (1 << 22))
#endif

static struct
{
	vao_t *vao;
	int vertexOffset;
	int indexOffset;

	int recycleVertex, recycleIndex; // codmod: counters for r_vaoCacheStats / r_speeds 9
}
vc;

// codmod: static cache. The stock cache copied every batch's vertexes and rebuilt its indexes whenever a set of
// surfaces had not been drawn before, which happens every frame while the view moves (tens of MB per frame on the
// bevelled maps; a still view re-uses its batches and uploads nothing). Now every surface's vertexes and (rebased)
// indexes are uploaded once, and a batch of surfaces is drawn with one glMultiDrawElements over their index ranges.
#define VAOCACHE_HASH_SIZE (1 << 18) // power of two
#define VAOCACHE_HASH_MAX (VAOCACHE_HASH_SIZE * 3 / 4)

typedef struct vtxEntry_s
{
	srfVert_t *verts;
	int numVerts;
	int base; // index of the first vertex in the vertex buffer
}
vtxEntry_t;

typedef struct idxEntry_s
{
	glIndex_t *indexes;
	srfVert_t *verts;
	int numIndexes;
	int byteOffset; // of the first (rebased) index in the index buffer
}
idxEntry_t;

static vtxEntry_t vtxTable[VAOCACHE_HASH_SIZE];
static idxEntry_t idxTable[VAOCACHE_HASH_SIZE];
static int vtxCount, idxCount;

#define VAOCACHE_MULTI_MAX VAOCACHE_QUEUE_MAX_SURFACES
static GLsizei mdCount[VAOCACHE_MULTI_MAX];
static const GLvoid *mdOffset[VAOCACHE_MULTI_MAX];
static int mdNum;

static unsigned int VaoCache_Hash(const void *p)
{
	return (unsigned int)(((size_t)p >> 4) * 2654435761u) & (VAOCACHE_HASH_SIZE - 1);
}

static void VaoCache_ClearTables(void)
{
	Com_Memset(vtxTable, 0, sizeof(vtxTable));
	Com_Memset(idxTable, 0, sizeof(idxTable));
	vtxCount = idxCount = 0;
}

// Index of the surface's first vertex in the cache buffer; copies the vertexes to the upload queue if they are new.
static int VaoCache_StaticVertexBase(srfVert_t *verts, int numVerts)
{
	unsigned int h = VaoCache_Hash(verts);
	vtxEntry_t *e;

	for (;;)
	{
		e = &vtxTable[h];
		if (!e->verts)
			break;
		if (e->verts == verts && e->numVerts == numVerts)
			return e->base;
		h = (h + 1) & (VAOCACHE_HASH_SIZE - 1);
	}

	e->verts = verts;
	e->numVerts = numVerts;
	e->base = (vc.vertexOffset + vcq.vertexCommitSize) / sizeof(srfVert_t);
	vtxCount++;
	Com_Memcpy((byte *)vcq.vertexes + vcq.vertexCommitSize, verts, numVerts * sizeof(srfVert_t));
	vcq.vertexCommitSize += numVerts * sizeof(srfVert_t);
	return e->base;
}

// Byte offset of the surface's rebased indexes in the cache buffer; copies them to the upload queue if they are new.
static int VaoCache_StaticIndexOffset(queuedSurface_t *surf, int vertexBase)
{
	unsigned int h = VaoCache_Hash(surf->indexes);
	idxEntry_t *e;
	glIndex_t *dst;
	int i;

	for (;;)
	{
		e = &idxTable[h];
		if (!e->indexes)
			break;
		if (e->indexes == surf->indexes && e->verts == surf->vertexes && e->numIndexes == surf->numIndexes)
			return e->byteOffset;
		h = (h + 1) & (VAOCACHE_HASH_SIZE - 1);
	}

	e->indexes = surf->indexes;
	e->verts = surf->vertexes;
	e->numIndexes = surf->numIndexes;
	e->byteOffset = vc.indexOffset + vcq.indexCommitSize;
	idxCount++;
	dst = (glIndex_t *)((byte *)vcq.indexes + vcq.indexCommitSize);
	for (i = 0; i < surf->numIndexes; i++)
		dst[i] = surf->indexes[i] + vertexBase;
	vcq.indexCommitSize += surf->numIndexes * sizeof(glIndex_t);
	return e->byteOffset;
}

void VaoCache_Reset(void)
{
	// the world's memory is gone after a map change: its vertex and index pointers must not match again
	VaoCache_ClearTables();
	vc.vertexOffset = 0;
	vc.indexOffset = 0;
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize = 0;
	vcq.numSurfaces = 0;
	mdNum = 0;
}

// Draws the batch of the last VaoCache_Commit (more than one surface)
void VaoCache_MultiDraw(void)
{
	qglMultiDrawElements(GL_TRIANGLES, mdCount, GL_INDEX_TYPE, mdOffset, mdNum);
}

void VaoCache_Commit(void)
{
	queuedSurface_t *surf, *end = vcq.surfaces + vcq.numSurfaces;

	R_BindVao(vc.vao);

	vcq.vertexCommitSize = 0; // from here on: bytes new to the buffer
	vcq.indexCommitSize = 0;
	mdNum = 0;
	for (surf = vcq.surfaces; surf < end; surf++)
	{
		int base = VaoCache_StaticVertexBase(surf->vertexes, surf->numVerts);
		int offset = VaoCache_StaticIndexOffset(surf, base);

		mdCount[mdNum] = surf->numIndexes;
		mdOffset[mdNum] = (const GLvoid *)(size_t)offset;
		mdNum++;
	}

	if (vcq.vertexCommitSize)
	{
		qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO);
		qglBufferSubData(GL_ARRAY_BUFFER, vc.vertexOffset, vcq.vertexCommitSize, vcq.vertexes);
		vc.vertexOffset += vcq.vertexCommitSize;
	}

	if (vcq.indexCommitSize)
	{
		qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO);
		qglBufferSubData(GL_ELEMENT_ARRAY_BUFFER, vc.indexOffset, vcq.indexCommitSize, vcq.indexes);
		vc.indexOffset += vcq.indexCommitSize;
	}

	if (mdNum == 1)
	{
		tess.firstIndex = (int)((size_t)mdOffset[0] / sizeof(glIndex_t));
		tess.numMultiDraws = 0;
	}
	else
	{
		tess.firstIndex = 0;
		tess.numMultiDraws = mdNum;
	}
}

void VaoCache_Init(void)
{
	vc.vao = R_CreateVao("VaoCache", NULL, VAOCACHE_VERTEX_BUFFER_SIZE, NULL, VAOCACHE_INDEX_BUFFER_SIZE, VAO_USAGE_DYNAMIC);

	vc.vao->attribs[ATTR_INDEX_POSITION].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].enabled       = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].enabled     = 1;
	vc.vao->attribs[ATTR_INDEX_NORMAL].enabled         = 1;
	vc.vao->attribs[ATTR_INDEX_TANGENT].enabled        = 1;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].enabled = 1;
	vc.vao->attribs[ATTR_INDEX_COLOR].enabled          = 1;

	vc.vao->attribs[ATTR_INDEX_POSITION].count       = 3;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].count       = 2;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].count     = 2;
	vc.vao->attribs[ATTR_INDEX_NORMAL].count         = 4;
	vc.vao->attribs[ATTR_INDEX_TANGENT].count        = 4;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].count = 4;
	vc.vao->attribs[ATTR_INDEX_COLOR].count          = 4;

	vc.vao->attribs[ATTR_INDEX_POSITION].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].type             = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].type           = GL_FLOAT;
	vc.vao->attribs[ATTR_INDEX_NORMAL].type               = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_TANGENT].type              = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].type       = GL_SHORT;
	vc.vao->attribs[ATTR_INDEX_COLOR].type                = GL_UNSIGNED_SHORT;

	vc.vao->attribs[ATTR_INDEX_POSITION].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].normalized       = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].normalized     = GL_FALSE;
	vc.vao->attribs[ATTR_INDEX_NORMAL].normalized         = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_TANGENT].normalized        = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].normalized = GL_TRUE;
	vc.vao->attribs[ATTR_INDEX_COLOR].normalized          = GL_TRUE;

	vc.vao->attribs[ATTR_INDEX_POSITION].offset       = offsetof(srfVert_t, xyz);
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].offset       = offsetof(srfVert_t, st);
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].offset     = offsetof(srfVert_t, lightmap);
	vc.vao->attribs[ATTR_INDEX_NORMAL].offset         = offsetof(srfVert_t, normal);
	vc.vao->attribs[ATTR_INDEX_TANGENT].offset        = offsetof(srfVert_t, tangent);
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].offset = offsetof(srfVert_t, lightdir);
	vc.vao->attribs[ATTR_INDEX_COLOR].offset          = offsetof(srfVert_t, color);

	vc.vao->attribs[ATTR_INDEX_POSITION].stride       = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_TEXCOORD].stride       = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_LIGHTCOORD].stride     = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_NORMAL].stride         = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_TANGENT].stride        = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_LIGHTDIRECTION].stride = sizeof(srfVert_t);
	vc.vao->attribs[ATTR_INDEX_COLOR].stride          = sizeof(srfVert_t);

	Vao_SetVertexPointers(vc.vao);

	vc.vertexOffset = 0;
	vc.indexOffset = 0;
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize = 0;
	vcq.numSurfaces = 0;
}

void VaoCache_BindVao(void)
{
	R_BindVao(vc.vao);
}

void VaoCache_CheckAdd(qboolean *endSurface, qboolean *recycleVertexBuffer, qboolean *recycleIndexBuffer, int numVerts, int numIndexes)
{
	int vertexesSize = sizeof(srfVert_t) * numVerts;
	int indexesSize = sizeof(glIndex_t) * numIndexes;

	if (vc.vao->vertexesSize < vc.vertexOffset + vcq.vertexCommitSize + vertexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of space in vertex cache: %d < %d + %d + %d\n", vc.vao->vertexesSize, vc.vertexOffset, vcq.vertexCommitSize, vertexesSize);
		*recycleVertexBuffer = qtrue;
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vc.vao->indexesSize < vc.indexOffset + vcq.indexCommitSize + indexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of space in index cache\n");
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vtxCount + vcq.numSurfaces >= VAOCACHE_HASH_MAX || idxCount + vcq.numSurfaces >= VAOCACHE_HASH_MAX)
	{
		//ri.Printf(PRINT_ALL, "out of surface table entries\n");
		*recycleVertexBuffer = qtrue;
		*recycleIndexBuffer = qtrue;
		*endSurface = qtrue;
	}

	if (vcq.numSurfaces >= VAOCACHE_QUEUE_MAX_SURFACES)
	{
		//ri.Printf(PRINT_ALL, "out of queued surfaces\n");
		*endSurface = qtrue;
	}

	if (VAOCACHE_QUEUE_MAX_VERTEXES * sizeof(srfVert_t) < vcq.vertexCommitSize + vertexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of queued vertexes\n");
		*endSurface = qtrue;
	}

	if (VAOCACHE_QUEUE_MAX_INDEXES * sizeof(glIndex_t) < vcq.indexCommitSize + indexesSize)
	{
		//ri.Printf(PRINT_ALL, "out of queued indexes\n");
		*endSurface = qtrue;
	}
}

// Vertexes and indexes are stored together (indexes are rebased to the vertex positions), so either recycle starts both over.
static void VaoCache_RecycleAll(void)
{
	qglBindBuffer(GL_ARRAY_BUFFER, vc.vao->vertexesVBO);
	qglBufferData(GL_ARRAY_BUFFER, vc.vao->vertexesSize, NULL, GL_DYNAMIC_DRAW);
	qglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesIBO);
	qglBufferData(GL_ELEMENT_ARRAY_BUFFER, vc.vao->indexesSize, NULL, GL_DYNAMIC_DRAW);
	vc.vertexOffset = 0;
	vc.indexOffset = 0;
	VaoCache_ClearTables();
}

void VaoCache_RecycleVertexBuffer(void)
{
	vc.recycleVertex++;
	VaoCache_RecycleAll();
}

void VaoCache_RecycleIndexBuffer(void)
{
	vc.recycleIndex++;
	VaoCache_RecycleAll();
}

// codmod: recycles since the last call (reset) and the current fill in percent
void VaoCache_GetStats(int *recycleVertex, int *recycleIndex, int *vertexFill, int *indexFill, int *numBatches)
{
	*recycleVertex = vc.recycleVertex;
	*recycleIndex = vc.recycleIndex;
	*vertexFill = (int)(100.0 * vc.vertexOffset / vc.vao->vertexesSize);
	*indexFill = (int)(100.0 * vc.indexOffset / vc.vao->indexesSize);
	*numBatches = idxCount;
	vc.recycleVertex = vc.recycleIndex = 0;
}

void VaoCache_InitQueue(void)
{
	vcq.vertexCommitSize = 0;
	vcq.indexCommitSize = 0;
	vcq.numSurfaces = 0;
}

void VaoCache_AddSurface(srfVert_t *verts, int numVerts, glIndex_t *indexes, int numIndexes)
{
	queuedSurface_t *queueEntry = vcq.surfaces + vcq.numSurfaces;
	queueEntry->vertexes = verts;
	queueEntry->numVerts = numVerts;
	queueEntry->indexes = indexes;
	queueEntry->numIndexes = numIndexes;
	vcq.numSurfaces++;

	vcq.vertexCommitSize += sizeof(srfVert_t) * numVerts;
	vcq.indexCommitSize += sizeof(glIndex_t) * numIndexes;
}
